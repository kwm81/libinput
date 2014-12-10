/*
 * Copyright © 2010 Intel Corporation
 * Copyright © 2013 Jonas Ådahl
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <sys/stat.h>

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "linux/input.h"
#include <unistd.h>
#include <fcntl.h>
#ifdef __linux__
#include <mtdev-plumbing.h>
#endif
#include <assert.h>
#include <time.h>
#include <math.h>

#include "libinput.h"
#include "evdev.h"
#include "filter.h"
#include "libinput-private.h"

#define DEFAULT_AXIS_STEP_DISTANCE 10
#define DEFAULT_MIDDLE_BUTTON_SCROLL_TIMEOUT 200

enum evdev_key_type {
	EVDEV_KEY_TYPE_NONE,
	EVDEV_KEY_TYPE_KEY,
	EVDEV_KEY_TYPE_BUTTON,
};

static void
hw_set_key_down(struct evdev_device *device, int code, int pressed)
{
	long_set_bit_state(device->hw_key_mask, code, pressed);
}

static int
hw_is_key_down(struct evdev_device *device, int code)
{
	return long_bit_is_set(device->hw_key_mask, code);
}

static int
get_key_down_count(struct evdev_device *device, int code)
{
	return device->key_count[code];
}

static int
update_key_down_count(struct evdev_device *device, int code, int pressed)
{
	int key_count;
	assert(code >= 0 && code < KEY_CNT);

	if (pressed) {
		key_count = ++device->key_count[code];
	} else {
		assert(device->key_count[code] > 0);
		key_count = --device->key_count[code];
	}

	if (key_count > 32) {
		log_bug_libinput(device->base.seat->libinput,
				 "Key count for %s reached abnormal values\n",
				 libevdev_event_code_get_name(EV_KEY, code));
	}

	return key_count;
}

void
evdev_keyboard_notify_key(struct evdev_device *device,
			  uint32_t time,
			  int key,
			  enum libinput_key_state state)
{
	int down_count;

	down_count = update_key_down_count(device, key, state);

	if ((state == LIBINPUT_KEY_STATE_PRESSED && down_count == 1) ||
	    (state == LIBINPUT_KEY_STATE_RELEASED && down_count == 0))
		keyboard_notify_key(&device->base, time, key, state);
}

void
evdev_pointer_notify_button(struct evdev_device *device,
			    uint32_t time,
			    int button,
			    enum libinput_button_state state)
{
	int down_count;

	down_count = update_key_down_count(device, button, state);

	if ((state == LIBINPUT_BUTTON_STATE_PRESSED && down_count == 1) ||
	    (state == LIBINPUT_BUTTON_STATE_RELEASED && down_count == 0)) {
		pointer_notify_button(&device->base, time, button, state);

		if (state == LIBINPUT_BUTTON_STATE_RELEASED &&
		    device->buttons.change_to_left_handed)
			device->buttons.change_to_left_handed(device);

		if (state == LIBINPUT_BUTTON_STATE_RELEASED &&
		    device->scroll.change_scroll_method)
			device->scroll.change_scroll_method(device);
	}

}

void
evdev_device_led_update(struct evdev_device *device, enum libinput_led leds)
{
	static const struct {
		enum libinput_led weston;
		int evdev;
	} map[] = {
		{ LIBINPUT_LED_NUM_LOCK, LED_NUML },
		{ LIBINPUT_LED_CAPS_LOCK, LED_CAPSL },
		{ LIBINPUT_LED_SCROLL_LOCK, LED_SCROLLL },
	};
	struct input_event ev[ARRAY_LENGTH(map) + 1];
	unsigned int i;

	if (!(device->seat_caps & EVDEV_DEVICE_KEYBOARD))
		return;

	memset(ev, 0, sizeof(ev));
	for (i = 0; i < ARRAY_LENGTH(map); i++) {
		ev[i].type = EV_LED;
		ev[i].code = map[i].evdev;
		ev[i].value = !!(leds & map[i].weston);
	}
	ev[i].type = EV_SYN;
	ev[i].code = SYN_REPORT;

	i = write(device->fd, ev, sizeof ev);
	(void)i; /* no, we really don't care about the return value */
}

static void
transform_absolute(struct evdev_device *device, int32_t *x, int32_t *y)
{
	if (!device->abs.apply_calibration)
		return;

	matrix_mult_vec(&device->abs.calibration, x, y);
}

static inline double
scale_axis(const struct input_absinfo *absinfo, double val, double to_range)
{
	return (val - absinfo->minimum) * to_range /
		(absinfo->maximum - absinfo->minimum + 1);
}

double
evdev_device_transform_x(struct evdev_device *device,
			 double x,
			 uint32_t width)
{
	return scale_axis(device->abs.absinfo_x, x, width);
}

double
evdev_device_transform_y(struct evdev_device *device,
			 double y,
			 uint32_t height)
{
	return scale_axis(device->abs.absinfo_y, y, height);
}

static void
evdev_flush_pending_event(struct evdev_device *device, uint64_t time)
{
	struct libinput *libinput = device->base.seat->libinput;
	struct motion_params motion;
	double dx_unaccel, dy_unaccel;
	int32_t cx, cy;
	int32_t x, y;
	int slot;
	int seat_slot;
	struct libinput_device *base = &device->base;
	struct libinput_seat *seat = base->seat;

	slot = device->mt.slot;

	switch (device->pending_event) {
	case EVDEV_NONE:
		return;
	case EVDEV_RELATIVE_MOTION:
		dx_unaccel = device->rel.dx / ((double) device->dpi /
					       DEFAULT_MOUSE_DPI);
		dy_unaccel = device->rel.dy / ((double) device->dpi /
					       DEFAULT_MOUSE_DPI);
		device->rel.dx = 0;
		device->rel.dy = 0;

		/* Use unaccelerated deltas for pointing stick scroll */
		if (device->scroll.method == LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN &&
		    hw_is_key_down(device, device->scroll.button)) {
			if (device->scroll.button_scroll_active)
				evdev_post_scroll(device, time,
						  dx_unaccel, dy_unaccel);
			break;
		}

		/* Apply pointer acceleration. */
		motion.dx = dx_unaccel;
		motion.dy = dy_unaccel;
		filter_dispatch(device->pointer.filter, &motion, device, time);

		if (motion.dx == 0.0 && motion.dy == 0.0 &&
		    dx_unaccel == 0.0 && dy_unaccel == 0.0) {
			break;
		}

		pointer_notify_motion(base, time,
				      motion.dx, motion.dy,
				      dx_unaccel, dy_unaccel);
		break;
	case EVDEV_ABSOLUTE_MT_DOWN:
		if (!(device->seat_caps & EVDEV_DEVICE_TOUCH))
			break;

		if (device->mt.slots[slot].seat_slot != -1) {
			log_bug_kernel(libinput,
				       "%s: Driver sent multiple touch down for the "
				       "same slot",
				       udev_device_get_devnode(device->udev_device));
			break;
		}

		seat_slot = ffs(~seat->slot_map) - 1;
		device->mt.slots[slot].seat_slot = seat_slot;

		if (seat_slot == -1)
			break;

		seat->slot_map |= 1 << seat_slot;
		x = device->mt.slots[slot].x;
		y = device->mt.slots[slot].y;
		transform_absolute(device, &x, &y);

		touch_notify_touch_down(base, time, slot, seat_slot, x, y);
		break;
	case EVDEV_ABSOLUTE_MT_MOTION:
		if (!(device->seat_caps & EVDEV_DEVICE_TOUCH))
			break;

		seat_slot = device->mt.slots[slot].seat_slot;
		x = device->mt.slots[slot].x;
		y = device->mt.slots[slot].y;

		if (seat_slot == -1)
			break;

		transform_absolute(device, &x, &y);
		touch_notify_touch_motion(base, time, slot, seat_slot, x, y);
		break;
	case EVDEV_ABSOLUTE_MT_UP:
		if (!(device->seat_caps & EVDEV_DEVICE_TOUCH))
			break;

		seat_slot = device->mt.slots[slot].seat_slot;
		device->mt.slots[slot].seat_slot = -1;

		if (seat_slot == -1)
			break;

		seat->slot_map &= ~(1 << seat_slot);

		touch_notify_touch_up(base, time, slot, seat_slot);
		break;
	case EVDEV_ABSOLUTE_TOUCH_DOWN:
		if (!(device->seat_caps & EVDEV_DEVICE_TOUCH))
			break;

		if (device->abs.seat_slot != -1) {
			log_bug_kernel(libinput,
				       "%s: Driver sent multiple touch down for the "
				       "same slot",
				       udev_device_get_devnode(device->udev_device));
			break;
		}

		seat_slot = ffs(~seat->slot_map) - 1;
		device->abs.seat_slot = seat_slot;

		if (seat_slot == -1)
			break;

		seat->slot_map |= 1 << seat_slot;

		cx = device->abs.x;
		cy = device->abs.y;
		transform_absolute(device, &cx, &cy);

		touch_notify_touch_down(base, time, -1, seat_slot, cx, cy);
		break;
	case EVDEV_ABSOLUTE_MOTION:
		cx = device->abs.x;
		cy = device->abs.y;
		transform_absolute(device, &cx, &cy);
		x = cx;
		y = cy;

		if (device->seat_caps & EVDEV_DEVICE_TOUCH) {
			seat_slot = device->abs.seat_slot;

			if (seat_slot == -1)
				break;

			touch_notify_touch_motion(base, time, -1, seat_slot, x, y);
		} else if (device->seat_caps & EVDEV_DEVICE_POINTER) {
			pointer_notify_motion_absolute(base, time, x, y);
		}
		break;
	case EVDEV_ABSOLUTE_TOUCH_UP:
		if (!(device->seat_caps & EVDEV_DEVICE_TOUCH))
			break;

		seat_slot = device->abs.seat_slot;
		device->abs.seat_slot = -1;

		if (seat_slot == -1)
			break;

		seat->slot_map &= ~(1 << seat_slot);

		touch_notify_touch_up(base, time, -1, seat_slot);
		break;
	default:
		assert(0 && "Unknown pending event type");
		break;
	}

	device->pending_event = EVDEV_NONE;
}

static enum evdev_key_type
get_key_type(uint16_t code)
{
	if (code == BTN_TOUCH)
		return EVDEV_KEY_TYPE_NONE;

	if (code >= KEY_ESC && code <= KEY_MICMUTE)
		return EVDEV_KEY_TYPE_KEY;
	if (code >= BTN_MISC && code <= BTN_GEAR_UP)
		return EVDEV_KEY_TYPE_BUTTON;
	if (code >= KEY_OK && code <= KEY_LIGHTS_TOGGLE)
		return EVDEV_KEY_TYPE_KEY;
	if (code >= BTN_DPAD_UP && code <= BTN_TRIGGER_HAPPY40)
		return EVDEV_KEY_TYPE_BUTTON;
	return EVDEV_KEY_TYPE_NONE;
}

static void
evdev_button_scroll_timeout(uint64_t time, void *data)
{
	struct evdev_device *device = data;

	device->scroll.button_scroll_active = true;
}

static void
evdev_button_scroll_button(struct evdev_device *device,
			   uint64_t time, int is_press)
{
	if (is_press) {
		libinput_timer_set(&device->scroll.timer,
				time + DEFAULT_MIDDLE_BUTTON_SCROLL_TIMEOUT);
	} else {
		libinput_timer_cancel(&device->scroll.timer);
		if (device->scroll.button_scroll_active) {
			evdev_stop_scroll(device, time);
			device->scroll.button_scroll_active = false;
		} else {
			/* If the button is released quickly enough emit the
			 * button press/release events. */
			evdev_pointer_notify_button(device, time,
					device->scroll.button,
					LIBINPUT_BUTTON_STATE_PRESSED);
			evdev_pointer_notify_button(device, time,
					device->scroll.button,
					LIBINPUT_BUTTON_STATE_RELEASED);
		}
	}
}

static void
evdev_process_touch_button(struct evdev_device *device,
			   uint64_t time, int value)
{
	if (device->pending_event != EVDEV_NONE &&
	    device->pending_event != EVDEV_ABSOLUTE_MOTION)
		evdev_flush_pending_event(device, time);

	device->pending_event = (value ?
				 EVDEV_ABSOLUTE_TOUCH_DOWN :
				 EVDEV_ABSOLUTE_TOUCH_UP);
}

static inline void
evdev_process_key(struct evdev_device *device,
		  struct input_event *e, uint64_t time)
{
	enum evdev_key_type type;

	/* ignore kernel key repeat */
	if (e->value == 2)
		return;

	if (e->code == BTN_TOUCH) {
		if (!device->is_mt)
			evdev_process_touch_button(device, time, e->value);
		return;
	}

	evdev_flush_pending_event(device, time);

	type = get_key_type(e->code);

	/* Ignore key release events from the kernel for keys that libinput
	 * never got a pressed event for. */
	if (e->value == 0) {
		switch (type) {
		case EVDEV_KEY_TYPE_NONE:
			break;
		case EVDEV_KEY_TYPE_KEY:
		case EVDEV_KEY_TYPE_BUTTON:
			if (!hw_is_key_down(device, e->code))
				return;
		}
	}

	hw_set_key_down(device, e->code, e->value);

	switch (type) {
	case EVDEV_KEY_TYPE_NONE:
		break;
	case EVDEV_KEY_TYPE_KEY:
		evdev_keyboard_notify_key(
			device,
			time,
			e->code,
			e->value ? LIBINPUT_KEY_STATE_PRESSED :
				   LIBINPUT_KEY_STATE_RELEASED);
		break;
	case EVDEV_KEY_TYPE_BUTTON:
		if (device->scroll.method == LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN &&
		    e->code == device->scroll.button) {
			evdev_button_scroll_button(device, time, e->value);
			break;
		}
		evdev_pointer_notify_button(
			device,
			time,
			evdev_to_left_handed(device, e->code),
			e->value ? LIBINPUT_BUTTON_STATE_PRESSED :
				   LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	}
}

static void
evdev_process_touch(struct evdev_device *device,
		    struct input_event *e,
		    uint64_t time)
{
	switch (e->code) {
	case ABS_MT_SLOT:
		evdev_flush_pending_event(device, time);
		device->mt.slot = e->value;
		break;
	case ABS_MT_TRACKING_ID:
		if (device->pending_event != EVDEV_NONE &&
		    device->pending_event != EVDEV_ABSOLUTE_MT_MOTION)
			evdev_flush_pending_event(device, time);
		if (e->value >= 0)
			device->pending_event = EVDEV_ABSOLUTE_MT_DOWN;
		else
			device->pending_event = EVDEV_ABSOLUTE_MT_UP;
		break;
	case ABS_MT_POSITION_X:
		device->mt.slots[device->mt.slot].x = e->value;
		if (device->pending_event == EVDEV_NONE)
			device->pending_event = EVDEV_ABSOLUTE_MT_MOTION;
		break;
	case ABS_MT_POSITION_Y:
		device->mt.slots[device->mt.slot].y = e->value;
		if (device->pending_event == EVDEV_NONE)
			device->pending_event = EVDEV_ABSOLUTE_MT_MOTION;
		break;
	}
}

static inline void
evdev_process_absolute_motion(struct evdev_device *device,
			      struct input_event *e)
{
	switch (e->code) {
	case ABS_X:
		device->abs.x = e->value;
		if (device->pending_event == EVDEV_NONE)
			device->pending_event = EVDEV_ABSOLUTE_MOTION;
		break;
	case ABS_Y:
		device->abs.y = e->value;
		if (device->pending_event == EVDEV_NONE)
			device->pending_event = EVDEV_ABSOLUTE_MOTION;
		break;
	}
}

static void
evdev_notify_axis(struct evdev_device *device,
		  uint64_t time,
		  enum libinput_pointer_axis axis,
		  double value)
{
	if (device->scroll.natural_scrolling_enabled)
		value *= -1;

	pointer_notify_axis(&device->base,
			    time,
			    axis,
			    value);
}

static inline void
evdev_process_relative(struct evdev_device *device,
		       struct input_event *e, uint64_t time)
{
	switch (e->code) {
	case REL_X:
		if (device->pending_event != EVDEV_RELATIVE_MOTION)
			evdev_flush_pending_event(device, time);
		device->rel.dx += e->value;
		device->pending_event = EVDEV_RELATIVE_MOTION;
		break;
	case REL_Y:
		if (device->pending_event != EVDEV_RELATIVE_MOTION)
			evdev_flush_pending_event(device, time);
		device->rel.dy += e->value;
		device->pending_event = EVDEV_RELATIVE_MOTION;
		break;
	case REL_WHEEL:
		evdev_flush_pending_event(device, time);
		evdev_notify_axis(
			device,
			time,
			LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL,
			-1 * e->value * DEFAULT_AXIS_STEP_DISTANCE);
		break;
	case REL_HWHEEL:
		evdev_flush_pending_event(device, time);
		evdev_notify_axis(
			device,
			time,
			LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL,
			e->value * DEFAULT_AXIS_STEP_DISTANCE);
		break;
	}
}

static inline void
evdev_process_absolute(struct evdev_device *device,
		       struct input_event *e,
		       uint64_t time)
{
	if (device->is_mt) {
		evdev_process_touch(device, e, time);
	} else {
		evdev_process_absolute_motion(device, e);
	}
}

static inline bool
evdev_any_button_down(struct evdev_device *device)
{
	unsigned int button;

	for (button = BTN_LEFT; button < BTN_JOYSTICK; button++) {
		if (libevdev_has_event_code(device->evdev, EV_KEY, button) &&
		    hw_is_key_down(device, button))
			return true;
	}
	return false;
}

static inline bool
evdev_need_touch_frame(struct evdev_device *device)
{
	if (!(device->seat_caps & EVDEV_DEVICE_TOUCH))
		return false;

	switch (device->pending_event) {
	case EVDEV_NONE:
	case EVDEV_RELATIVE_MOTION:
		break;
	case EVDEV_ABSOLUTE_MT_DOWN:
	case EVDEV_ABSOLUTE_MT_MOTION:
	case EVDEV_ABSOLUTE_MT_UP:
	case EVDEV_ABSOLUTE_TOUCH_DOWN:
	case EVDEV_ABSOLUTE_TOUCH_UP:
	case EVDEV_ABSOLUTE_MOTION:
		return true;
	}

	return false;
}

static void
evdev_tag_external_mouse(struct evdev_device *device,
			 struct udev_device *udev_device)
{
	int bustype;

	bustype = libevdev_get_id_bustype(device->evdev);
	if (bustype == BUS_USB || bustype == BUS_BLUETOOTH) {
		if (device->seat_caps & EVDEV_DEVICE_POINTER)
			device->tags |= EVDEV_TAG_EXTERNAL_MOUSE;
	}
}

static void
evdev_tag_trackpoint(struct evdev_device *device,
		     struct udev_device *udev_device)
{
	if (libevdev_has_property(device->evdev, INPUT_PROP_POINTING_STICK))
		device->tags |= EVDEV_TAG_TRACKPOINT;
}

static void
fallback_process(struct evdev_dispatch *dispatch,
		 struct evdev_device *device,
		 struct input_event *event,
		 uint64_t time)
{
	bool need_frame = false;

	switch (event->type) {
	case EV_REL:
		evdev_process_relative(device, event, time);
		break;
	case EV_ABS:
		evdev_process_absolute(device, event, time);
		break;
	case EV_KEY:
		evdev_process_key(device, event, time);
		break;
	case EV_SYN:
		need_frame = evdev_need_touch_frame(device);
		evdev_flush_pending_event(device, time);
		if (need_frame)
			touch_notify_frame(&device->base, time);
		break;
	}
}

static void
fallback_destroy(struct evdev_dispatch *dispatch)
{
	free(dispatch);
}

static void
fallback_tag_device(struct evdev_device *device,
		    struct udev_device *udev_device)
{
	evdev_tag_external_mouse(device, udev_device);
	evdev_tag_trackpoint(device, udev_device);
}

static int
evdev_calibration_has_matrix(struct libinput_device *libinput_device)
{
	struct evdev_device *device = (struct evdev_device*)libinput_device;

	return device->abs.absinfo_x && device->abs.absinfo_y;
}

static enum libinput_config_status
evdev_calibration_set_matrix(struct libinput_device *libinput_device,
			     const float matrix[6])
{
	struct evdev_device *device = (struct evdev_device*)libinput_device;

	evdev_device_calibrate(device, matrix);

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static int
evdev_calibration_get_matrix(struct libinput_device *libinput_device,
			     float matrix[6])
{
	struct evdev_device *device = (struct evdev_device*)libinput_device;

	matrix_to_farray6(&device->abs.usermatrix, matrix);

	return !matrix_is_identity(&device->abs.usermatrix);
}

static int
evdev_calibration_get_default_matrix(struct libinput_device *libinput_device,
				     float matrix[6])
{
	struct evdev_device *device = (struct evdev_device*)libinput_device;

	matrix_to_farray6(&device->abs.default_calibration, matrix);

	return !matrix_is_identity(&device->abs.default_calibration);
}

struct evdev_dispatch_interface fallback_interface = {
	fallback_process,
	NULL, /* remove */
	fallback_destroy,
	NULL, /* device_added */
	NULL, /* device_removed */
	NULL, /* device_suspended */
	NULL, /* device_resumed */
	fallback_tag_device,
};

static uint32_t
evdev_sendevents_get_modes(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_SEND_EVENTS_DISABLED;
}

static enum libinput_config_status
evdev_sendevents_set_mode(struct libinput_device *device,
			  enum libinput_config_send_events_mode mode)
{
	struct evdev_device *evdev = (struct evdev_device*)device;
	struct evdev_dispatch *dispatch = evdev->dispatch;

	if (mode == dispatch->sendevents.current_mode)
		return LIBINPUT_CONFIG_STATUS_SUCCESS;

	switch(mode) {
	case LIBINPUT_CONFIG_SEND_EVENTS_ENABLED:
		evdev_device_resume(evdev);
		break;
	case LIBINPUT_CONFIG_SEND_EVENTS_DISABLED:
		evdev_device_suspend(evdev);
		break;
	default: /* no support for combined modes yet */
		return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
	}

	dispatch->sendevents.current_mode = mode;

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static enum libinput_config_send_events_mode
evdev_sendevents_get_mode(struct libinput_device *device)
{
	struct evdev_device *evdev = (struct evdev_device*)device;
	struct evdev_dispatch *dispatch = evdev->dispatch;

	return dispatch->sendevents.current_mode;
}

static enum libinput_config_send_events_mode
evdev_sendevents_get_default_mode(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
}

static int
evdev_left_handed_has(struct libinput_device *device)
{
	/* This is only hooked up when we have left-handed configuration, so we
	 * can hardcode 1 here */
	return 1;
}

static void
evdev_change_to_left_handed(struct evdev_device *device)
{
	if (device->buttons.want_left_handed == device->buttons.left_handed)
		return;

	if (evdev_any_button_down(device))
		return;

	device->buttons.left_handed = device->buttons.want_left_handed;
}

static enum libinput_config_status
evdev_left_handed_set(struct libinput_device *device, int left_handed)
{
	struct evdev_device *evdev_device = (struct evdev_device *)device;

	evdev_device->buttons.want_left_handed = left_handed ? true : false;

	evdev_device->buttons.change_to_left_handed(evdev_device);

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static int
evdev_left_handed_get(struct libinput_device *device)
{
	struct evdev_device *evdev_device = (struct evdev_device *)device;

	/* return the wanted configuration, even if it hasn't taken
	 * effect yet! */
	return evdev_device->buttons.want_left_handed;
}

static int
evdev_left_handed_get_default(struct libinput_device *device)
{
	return 0;
}

int
evdev_init_left_handed(struct evdev_device *device,
		       void (*change_to_left_handed)(struct evdev_device *))
{
	device->buttons.config_left_handed.has = evdev_left_handed_has;
	device->buttons.config_left_handed.set = evdev_left_handed_set;
	device->buttons.config_left_handed.get = evdev_left_handed_get;
	device->buttons.config_left_handed.get_default = evdev_left_handed_get_default;
	device->base.config.left_handed = &device->buttons.config_left_handed;
	device->buttons.left_handed = false;
	device->buttons.want_left_handed = false;
	device->buttons.change_to_left_handed = change_to_left_handed;

	return 0;
}

static uint32_t
evdev_scroll_get_methods(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
}

static void
evdev_change_scroll_method(struct evdev_device *device)
{
	if (device->scroll.want_method == device->scroll.method &&
	    device->scroll.want_button == device->scroll.button)
		return;

	if (evdev_any_button_down(device))
		return;

	device->scroll.method = device->scroll.want_method;
	device->scroll.button = device->scroll.want_button;
}

static enum libinput_config_status
evdev_scroll_set_method(struct libinput_device *device,
			enum libinput_config_scroll_method method)
{
	struct evdev_device *evdev = (struct evdev_device*)device;

	evdev->scroll.want_method = method;
	evdev->scroll.change_scroll_method(evdev);

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static enum libinput_config_scroll_method
evdev_scroll_get_method(struct libinput_device *device)
{
	struct evdev_device *evdev = (struct evdev_device *)device;

	/* return the wanted configuration, even if it hasn't taken
	 * effect yet! */
	return evdev->scroll.want_method;
}

static enum libinput_config_scroll_method
evdev_scroll_get_default_method(struct libinput_device *device)
{
	struct evdev_device *evdev = (struct evdev_device *)device;

	if (libevdev_has_property(evdev->evdev, INPUT_PROP_POINTING_STICK))
		return LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
	else
		return LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
}

static enum libinput_config_status
evdev_scroll_set_button(struct libinput_device *device,
			uint32_t button)
{
	struct evdev_device *evdev = (struct evdev_device*)device;

	evdev->scroll.want_button = button;
	evdev->scroll.change_scroll_method(evdev);

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static uint32_t
evdev_scroll_get_button(struct libinput_device *device)
{
	struct evdev_device *evdev = (struct evdev_device *)device;

	/* return the wanted configuration, even if it hasn't taken
	 * effect yet! */
	return evdev->scroll.want_button;
}

static uint32_t
evdev_scroll_get_default_button(struct libinput_device *device)
{
	struct evdev_device *evdev = (struct evdev_device *)device;

	if (libevdev_has_property(evdev->evdev, INPUT_PROP_POINTING_STICK))
		return BTN_MIDDLE;
	else
		return 0;
}

static int
evdev_init_button_scroll(struct evdev_device *device,
			 void (*change_scroll_method)(struct evdev_device *))
{
	libinput_timer_init(&device->scroll.timer, device->base.seat->libinput,
			    evdev_button_scroll_timeout, device);
	device->scroll.config.get_methods = evdev_scroll_get_methods;
	device->scroll.config.set_method = evdev_scroll_set_method;
	device->scroll.config.get_method = evdev_scroll_get_method;
	device->scroll.config.get_default_method = evdev_scroll_get_default_method;
	device->scroll.config.set_button = evdev_scroll_set_button;
	device->scroll.config.get_button = evdev_scroll_get_button;
	device->scroll.config.get_default_button = evdev_scroll_get_default_button;
	device->base.config.scroll_method = &device->scroll.config;
	device->scroll.method = evdev_scroll_get_default_method((struct libinput_device *)device);
	device->scroll.want_method = device->scroll.method;
	device->scroll.button = evdev_scroll_get_default_button((struct libinput_device *)device);
	device->scroll.want_button = device->scroll.button;
	device->scroll.change_scroll_method = change_scroll_method;

	return 0;
}

static void
evdev_init_calibration(struct evdev_device *device,
		       struct evdev_dispatch *dispatch)
{
	device->base.config.calibration = &dispatch->calibration;

	dispatch->calibration.has_matrix = evdev_calibration_has_matrix;
	dispatch->calibration.set_matrix = evdev_calibration_set_matrix;
	dispatch->calibration.get_matrix = evdev_calibration_get_matrix;
	dispatch->calibration.get_default_matrix = evdev_calibration_get_default_matrix;
}

static void
evdev_init_sendevents(struct evdev_device *device,
		      struct evdev_dispatch *dispatch)
{
	device->base.config.sendevents = &dispatch->sendevents.config;

	dispatch->sendevents.current_mode = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
	dispatch->sendevents.config.get_modes = evdev_sendevents_get_modes;
	dispatch->sendevents.config.set_mode = evdev_sendevents_set_mode;
	dispatch->sendevents.config.get_mode = evdev_sendevents_get_mode;
	dispatch->sendevents.config.get_default_mode = evdev_sendevents_get_default_mode;
}

static int
evdev_scroll_config_natural_has(struct libinput_device *device)
{
	return 1;
}

static enum libinput_config_status
evdev_scroll_config_natural_set(struct libinput_device *device,
				int enabled)
{
	struct evdev_device *dev = (struct evdev_device *)device;

	dev->scroll.natural_scrolling_enabled = enabled ? true : false;

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static int
evdev_scroll_config_natural_get(struct libinput_device *device)
{
	struct evdev_device *dev = (struct evdev_device *)device;

	return dev->scroll.natural_scrolling_enabled ? 1 : 0;
}

static int
evdev_scroll_config_natural_get_default(struct libinput_device *device)
{
	/* could enable this on Apple touchpads. could do that, could
	 * very well do that... */
	return 0;
}

void
evdev_init_natural_scroll(struct evdev_device *device)
{
	device->scroll.config_natural.has = evdev_scroll_config_natural_has;
	device->scroll.config_natural.set_enabled = evdev_scroll_config_natural_set;
	device->scroll.config_natural.get_enabled = evdev_scroll_config_natural_get;
	device->scroll.config_natural.get_default_enabled = evdev_scroll_config_natural_get_default;
	device->scroll.natural_scrolling_enabled = false;
	device->base.config.natural_scroll = &device->scroll.config_natural;
}

static struct evdev_dispatch *
fallback_dispatch_create(struct libinput_device *device)
{
	struct evdev_dispatch *dispatch = zalloc(sizeof *dispatch);
	struct evdev_device *evdev_device = (struct evdev_device *)device;

	if (dispatch == NULL)
		return NULL;

	dispatch->interface = &fallback_interface;

	if (evdev_device->buttons.want_left_handed &&
	    evdev_init_left_handed(evdev_device,
				   evdev_change_to_left_handed) == -1) {
		free(dispatch);
		return NULL;
	}

	if (evdev_device->scroll.want_button &&
	    evdev_init_button_scroll(evdev_device,
				     evdev_change_scroll_method) == -1) {
		free(dispatch);
		return NULL;
	}

	if (evdev_device->scroll.natural_scrolling_enabled)
		evdev_init_natural_scroll(evdev_device);

	evdev_init_calibration(evdev_device, dispatch);
	evdev_init_sendevents(evdev_device, dispatch);

	return dispatch;
}

static inline void
evdev_process_event(struct evdev_device *device, struct input_event *e)
{
	struct evdev_dispatch *dispatch = device->dispatch;
	uint64_t time = e->time.tv_sec * 1000ULL + e->time.tv_usec / 1000;

	dispatch->interface->process(dispatch, device, e, time);
}

static inline void
evdev_device_dispatch_one(struct evdev_device *device,
			  struct input_event *ev)
{
#ifdef __linux__
	if (!device->mtdev) {
		evdev_process_event(device, ev);
	} else {
		mtdev_put_event(device->mtdev, ev);
		if (libevdev_event_is_code(ev, EV_SYN, SYN_REPORT)) {
			while (!mtdev_empty(device->mtdev)) {
				struct input_event e;
				mtdev_get_event(device->mtdev, &e);
				evdev_process_event(device, &e);
			}
		}
	}
#else
	evdev_process_event(device, ev);
#endif
}

static int
evdev_sync_device(struct evdev_device *device)
{
	struct input_event ev;
	int rc;

	do {
		rc = libevdev_next_event(device->evdev,
					 LIBEVDEV_READ_FLAG_SYNC, &ev);
		if (rc < 0)
			break;
		evdev_device_dispatch_one(device, &ev);
	} while (rc == LIBEVDEV_READ_STATUS_SYNC);

	return rc == -EAGAIN ? 0 : rc;
}

static void
evdev_device_dispatch(void *data)
{
	struct evdev_device *device = data;
	struct libinput *libinput = device->base.seat->libinput;
	struct input_event ev;
	int rc;

	/* If the compositor is repainting, this function is called only once
	 * per frame and we have to process all the events available on the
	 * fd, otherwise there will be input lag. */
	do {
		rc = libevdev_next_event(device->evdev,
					 LIBEVDEV_READ_FLAG_NORMAL, &ev);
		if (rc == LIBEVDEV_READ_STATUS_SYNC) {
			switch (ratelimit_test(&device->syn_drop_limit)) {
			case RATELIMIT_PASS:
				log_info(libinput, "SYN_DROPPED event from "
					 "\"%s\" - some input events have "
					 "been lost.\n", device->devname);
				break;
			case RATELIMIT_THRESHOLD:
				log_info(libinput, "SYN_DROPPED flood "
					 "from \"%s\"\n",
					 device->devname);
				break;
			case RATELIMIT_EXCEEDED:
				break;
			}

			/* send one more sync event so we handle all
			   currently pending events before we sync up
			   to the current state */
			ev.code = SYN_REPORT;
			evdev_device_dispatch_one(device, &ev);

			rc = evdev_sync_device(device);
			if (rc == 0)
				rc = LIBEVDEV_READ_STATUS_SUCCESS;
		} else if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
			evdev_device_dispatch_one(device, &ev);
		}
	} while (rc == LIBEVDEV_READ_STATUS_SUCCESS);

	if (rc != -EAGAIN && rc != -EINTR) {
		libinput_remove_source(libinput, device->source);
		device->source = NULL;
	}
}

static int
evdev_accel_config_available(struct libinput_device *device)
{
	/* this function is only called if we set up ptraccel, so we can
	   reply with a resounding "Yes" */
	return 1;
}

static enum libinput_config_status
evdev_accel_config_set_speed(struct libinput_device *device, double speed)
{
	struct evdev_device *dev = (struct evdev_device *)device;

	if (!filter_set_speed(dev->pointer.filter, speed))
		return LIBINPUT_CONFIG_STATUS_INVALID;

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static double
evdev_accel_config_get_speed(struct libinput_device *device)
{
	struct evdev_device *dev = (struct evdev_device *)device;

	return filter_get_speed(dev->pointer.filter);
}

static double
evdev_accel_config_get_default_speed(struct libinput_device *device)
{
	return 0.0;
}

int
evdev_device_init_pointer_acceleration(struct evdev_device *device)
{
	device->pointer.filter =
		create_pointer_accelerator_filter(
			pointer_accel_profile_linear);
	if (!device->pointer.filter)
		return -1;

	device->pointer.config.available = evdev_accel_config_available;
	device->pointer.config.set_speed = evdev_accel_config_set_speed;
	device->pointer.config.get_speed = evdev_accel_config_get_speed;
	device->pointer.config.get_default_speed = evdev_accel_config_get_default_speed;
	device->base.config.accel = &device->pointer.config;

	return 0;
}


#ifdef __linux__
static inline int
evdev_need_mtdev(struct evdev_device *device)
{
	struct libevdev *evdev = device->evdev;

	return (libevdev_has_event_code(evdev, EV_ABS, ABS_MT_POSITION_X) &&
		libevdev_has_event_code(evdev, EV_ABS, ABS_MT_POSITION_Y) &&
		!libevdev_has_event_code(evdev, EV_ABS, ABS_MT_SLOT));
}
#endif

static void
evdev_tag_device(struct evdev_device *device)
{
	if (device->dispatch->interface->tag_device)
		device->dispatch->interface->tag_device(device,
							device->udev_device);
}

static inline int
evdev_read_dpi_prop(struct evdev_device *device)
{
	struct libinput *libinput = device->base.seat->libinput;
	const char *mouse_dpi;
	int dpi = DEFAULT_MOUSE_DPI;

	mouse_dpi = udev_device_get_property_value(device->udev_device,
						   "MOUSE_DPI");
	if (mouse_dpi) {
		dpi = parse_mouse_dpi_property(mouse_dpi);
		if (!dpi) {
			log_error(libinput, "Mouse DPI property for '%s' is "
					    "present but invalid, using %d "
					    "DPI instead\n",
					    device->devname,
					    DEFAULT_MOUSE_DPI);
			dpi = DEFAULT_MOUSE_DPI;
		}
	}

	return dpi;
}

static inline int
evdev_fix_abs_resolution(struct libevdev *evdev,
			 unsigned int code,
			 const struct input_absinfo *absinfo)
{
	struct input_absinfo fixed;

	if (absinfo->resolution == 0) {
		fixed = *absinfo;
		fixed.resolution = 1;
		/* libevdev_set_abs_info() changes the absinfo we already
		   have a pointer to, no need to fetch it again */
		libevdev_set_abs_info(evdev, code, &fixed);
		return 1;
	} else {
		return 0;
	}
}

static int
evdev_configure_device(struct evdev_device *device)
{
	struct libinput *libinput = device->base.seat->libinput;
	struct libevdev *evdev = device->evdev;
	const struct input_absinfo *absinfo;
	int has_abs, has_rel, has_mt;
	int has_button, has_keyboard, has_touch;
	struct mt_slot *slots;
	int num_slots;
	int active_slot;
	int slot;
	unsigned int i;
	const char *devnode = udev_device_get_devnode(device->udev_device);

	has_rel = 0;
	has_abs = 0;
	has_mt = 0;
	has_button = 0;
	has_keyboard = 0;
	has_touch = 0;

        for (i = BTN_JOYSTICK; i < BTN_DIGI; i++) {
                if (libevdev_has_event_code(evdev, EV_KEY, i)) {
                        log_info(libinput,
                                 "input device '%s', %s is a joystick, ignoring\n",
                                 device->devname, devnode);
                        return -1;
                }
        }

	if (libevdev_has_event_type(evdev, EV_ABS)) {

		if ((absinfo = libevdev_get_abs_info(evdev, ABS_X))) {
			if (evdev_fix_abs_resolution(evdev,
						     ABS_X,
						     absinfo))
				device->abs.fake_resolution = 1;
			device->abs.absinfo_x = absinfo;
			has_abs = 1;
		}
		if ((absinfo = libevdev_get_abs_info(evdev, ABS_Y))) {
			if (evdev_fix_abs_resolution(evdev,
						     ABS_Y,
						     absinfo))
				device->abs.fake_resolution = 1;
			device->abs.absinfo_y = absinfo;
			has_abs = 1;
		}

		/* Fake MT devices have the ABS_MT_SLOT bit set because of
		   the limited ABS_* range - they aren't MT devices, they
		   just have too many ABS_ axes */
		if (libevdev_has_event_code(evdev, EV_ABS, ABS_MT_SLOT) &&
		    libevdev_get_num_slots(evdev) == -1) {
			has_mt = 0;
			has_touch = 0;
		} else if (libevdev_has_event_code(evdev, EV_ABS, ABS_MT_POSITION_X) &&
			   libevdev_has_event_code(evdev, EV_ABS, ABS_MT_POSITION_Y)) {
			absinfo = libevdev_get_abs_info(evdev, ABS_MT_POSITION_X);
			if (evdev_fix_abs_resolution(evdev,
						     ABS_MT_POSITION_X,
						     absinfo))
				device->abs.fake_resolution = 1;
			device->abs.absinfo_x = absinfo;

			absinfo = libevdev_get_abs_info(evdev, ABS_MT_POSITION_Y);
			if (evdev_fix_abs_resolution(evdev,
						     ABS_MT_POSITION_Y,
						     absinfo))
				device->abs.fake_resolution = 1;
			device->abs.absinfo_y = absinfo;
			device->is_mt = 1;
			has_touch = 1;
			has_mt = 1;

			/* We only handle the slotted Protocol B in libinput.
			   Devices with ABS_MT_POSITION_* but not ABS_MT_SLOT
			   require mtdev for conversion. */
#ifdef __linux__
			if (evdev_need_mtdev(device)) {
				device->mtdev = mtdev_new_open(device->fd);
				if (!device->mtdev)
					return -1;

				num_slots = device->mtdev->caps.slot.maximum;
				if (device->mtdev->caps.slot.minimum < 0 ||
				    num_slots <= 0)
					return -1;
				active_slot = device->mtdev->caps.slot.value;
			} else {
#endif
				num_slots = libevdev_get_num_slots(device->evdev);
				active_slot = libevdev_get_current_slot(evdev);
#ifdef __linux__
			}
#endif

			slots = calloc(num_slots, sizeof(struct mt_slot));
			if (!slots)
				return -1;

			for (slot = 0; slot < num_slots; ++slot) {
				slots[slot].seat_slot = -1;
				slots[slot].x = 0;
				slots[slot].y = 0;
			}
			device->mt.slots = slots;
			device->mt.slots_len = num_slots;
			device->mt.slot = active_slot;
		}
	}

	if (libevdev_has_event_code(evdev, EV_REL, REL_X) ||
	    libevdev_has_event_code(evdev, EV_REL, REL_Y))
		has_rel = 1;

	if (libevdev_has_event_type(evdev, EV_KEY)) {
		if (!libevdev_has_property(evdev, INPUT_PROP_DIRECT) &&
		    libevdev_has_event_code(evdev, EV_KEY, BTN_TOOL_FINGER) &&
		    !libevdev_has_event_code(evdev, EV_KEY, BTN_TOOL_PEN) &&
		    (has_abs || has_mt)) {
			device->dispatch = evdev_mt_touchpad_create(device);
			log_info(libinput,
				 "input device '%s', %s is a touchpad\n",
				 device->devname, devnode);
			return device->dispatch == NULL ? -1 : 0;
		}

		for (i = 0; i < KEY_MAX; i++) {
			if (libevdev_has_event_code(evdev, EV_KEY, i)) {
				switch (get_key_type(i)) {
				case EVDEV_KEY_TYPE_NONE:
					break;
				case EVDEV_KEY_TYPE_KEY:
					has_keyboard = 1;
					break;
				case EVDEV_KEY_TYPE_BUTTON:
					has_button = 1;
					break;
				}
			}
		}

		if (libevdev_has_event_code(evdev, EV_KEY, BTN_TOUCH))
			has_touch = 1;
	}
	if (libevdev_has_event_type(evdev, EV_LED))
		has_keyboard = 1;

	if ((has_abs || has_rel) && has_button) {
		if (evdev_device_init_pointer_acceleration(device) == -1)
			return -1;

		device->seat_caps |= EVDEV_DEVICE_POINTER;

		log_info(libinput,
			 "input device '%s', %s is a pointer caps =%s%s%s\n",
			 device->devname, devnode,
			 has_abs ? " absolute-motion" : "",
			 has_rel ? " relative-motion": "",
			 has_button ? " button" : "");

		/* want left-handed config option */
		device->buttons.want_left_handed = true;
		/* want natural-scroll config option */
		device->scroll.natural_scrolling_enabled = true;
	}

	if (has_rel && has_button) {
		/* want button scrolling config option */
		device->scroll.want_button = 1;
	}

	if (has_keyboard) {
		device->seat_caps |= EVDEV_DEVICE_KEYBOARD;
		log_info(libinput,
			 "input device '%s', %s is a keyboard\n",
			 device->devname, devnode);
	}
	if (has_touch && !has_button) {
		device->seat_caps |= EVDEV_DEVICE_TOUCH;
		log_info(libinput,
			 "input device '%s', %s is a touch device\n",
			 device->devname, devnode);
	}

	return 0;
}

static void
evdev_notify_added_device(struct evdev_device *device)
{
	struct libinput_device *dev;

	list_for_each(dev, &device->base.seat->devices_list, link) {
		struct evdev_device *d = (struct evdev_device*)dev;
		if (dev == &device->base)
			continue;

		/* Notify existing device d about addition of device device */
		if (d->dispatch->interface->device_added)
			d->dispatch->interface->device_added(d, device);

		/* Notify new device device about existing device d */
		if (device->dispatch->interface->device_added)
			device->dispatch->interface->device_added(device, d);

		/* Notify new device device if existing device d is suspended */
		if (d->suspended && device->dispatch->interface->device_suspended)
			device->dispatch->interface->device_suspended(device, d);
	}

	notify_added_device(&device->base);
}

static int
evdev_device_compare_syspath(struct udev_device *udev_device, int fd)
{
	struct udev *udev = udev_device_get_udev(udev_device);
	struct udev_device *udev_device_new = NULL;
	struct stat st;
	int rc = 1;

	if (fstat(fd, &st) < 0)
		goto out;

	udev_device_new = udev_device_new_from_devnum(udev, 'c', st.st_rdev);
	if (!udev_device_new)
		goto out;

	rc = strcmp(udev_device_get_syspath(udev_device_new),
		    udev_device_get_syspath(udev_device));
out:
	if (udev_device_new)
		udev_device_unref(udev_device_new);
	return rc;
}

struct evdev_device *
evdev_device_create(struct libinput_seat *seat,
		    struct udev_device *udev_device)
{
	struct libinput *libinput = seat->libinput;
	struct evdev_device *device = NULL;
	int rc;
	int fd;
	int unhandled_device = 0;
	const char *devnode = udev_device_get_devnode(udev_device);

	/* Use non-blocking mode so that we can loop on read on
	 * evdev_device_data() until all events on the fd are
	 * read.  mtdev_get() also expects this. */
	fd = open_restricted(libinput, devnode, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		log_info(libinput,
			 "opening input device '%s' failed (%s).\n",
			 devnode, strerror(-fd));
		return NULL;
	}

	if (evdev_device_compare_syspath(udev_device, fd) != 0)
		goto err;

	device = zalloc(sizeof *device);
	if (device == NULL)
		goto err;

	libinput_device_init(&device->base, seat);
	libinput_seat_ref(seat);

	rc = libevdev_new_from_fd(fd, &device->evdev);
	if (rc != 0)
		goto err;

	libevdev_set_clock_id(device->evdev, CLOCK_MONOTONIC);

	device->seat_caps = 0;
	device->is_mt = 0;
	device->mtdev = NULL;
	device->udev_device = udev_device_ref(udev_device);
	device->rel.dx = 0;
	device->rel.dy = 0;
	device->abs.seat_slot = -1;
	device->dispatch = NULL;
	device->fd = fd;
	device->pending_event = EVDEV_NONE;
	device->devname = libevdev_get_name(device->evdev);
	device->scroll.threshold = 5.0; /* Default may be overridden */
	device->scroll.direction = 0;
	device->dpi = evdev_read_dpi_prop(device);
	/* at most 5 SYN_DROPPED log-messages per 30s */
	ratelimit_init(&device->syn_drop_limit, 30ULL * 1000, 5);

	matrix_init_identity(&device->abs.calibration);
	matrix_init_identity(&device->abs.usermatrix);
	matrix_init_identity(&device->abs.default_calibration);

	if (evdev_configure_device(device) == -1)
		goto err;

	if (device->seat_caps == 0) {
		unhandled_device = 1;
		goto err;
	}

	/* If the dispatch was not set up use the fallback. */
	if (device->dispatch == NULL)
		device->dispatch = fallback_dispatch_create(&device->base);
	if (device->dispatch == NULL)
		goto err;

	device->source =
		libinput_add_fd(libinput, fd, evdev_device_dispatch, device);
	if (!device->source)
		goto err;

	list_insert(seat->devices_list.prev, &device->base.link);

	evdev_tag_device(device);
	evdev_notify_added_device(device);

	return device;

err:
	if (fd >= 0)
		close_restricted(libinput, fd);
	if (device)
		evdev_device_destroy(device);

	return unhandled_device ? EVDEV_UNHANDLED_DEVICE :  NULL;
}

int
evdev_device_get_keys(struct evdev_device *device, char *keys, size_t size)
{
	memset(keys, 0, size);
	return 0;
}

const char *
evdev_device_get_output(struct evdev_device *device)
{
	return device->output_name;
}

const char *
evdev_device_get_sysname(struct evdev_device *device)
{
	return udev_device_get_sysname(device->udev_device);
}

const char *
evdev_device_get_name(struct evdev_device *device)
{
	return device->devname;
}

unsigned int
evdev_device_get_id_product(struct evdev_device *device)
{
	return libevdev_get_id_product(device->evdev);
}

unsigned int
evdev_device_get_id_vendor(struct evdev_device *device)
{
	return libevdev_get_id_vendor(device->evdev);
}

struct udev_device *
evdev_device_get_udev_device(struct evdev_device *device)
{
	return udev_device_ref(device->udev_device);
}

void
evdev_device_set_default_calibration(struct evdev_device *device,
				     const float calibration[6])
{
	matrix_from_farray6(&device->abs.default_calibration, calibration);
	evdev_device_calibrate(device, calibration);
}

void
evdev_device_calibrate(struct evdev_device *device,
		       const float calibration[6])
{
	struct matrix scale,
		      translate,
		      transform;
	double sx, sy;

	matrix_from_farray6(&transform, calibration);
	device->abs.apply_calibration = !matrix_is_identity(&transform);

	if (!device->abs.apply_calibration) {
		matrix_init_identity(&device->abs.calibration);
		return;
	}

	sx = device->abs.absinfo_x->maximum - device->abs.absinfo_x->minimum + 1;
	sy = device->abs.absinfo_y->maximum - device->abs.absinfo_y->minimum + 1;

	/* The transformation matrix is in the form:
	 *  [ a b c ]
	 *  [ d e f ]
	 *  [ 0 0 1 ]
	 * Where a, e are the scale components, a, b, d, e are the rotation
	 * component (combined with scale) and c and f are the translation
	 * component. The translation component in the input matrix must be
	 * normalized to multiples of the device width and height,
	 * respectively. e.g. c == 1 shifts one device-width to the right.
	 *
	 * We pre-calculate a single matrix to apply to event coordinates:
	 *     M = Un-Normalize * Calibration * Normalize
	 *
	 * Normalize: scales the device coordinates to [0,1]
	 * Calibration: user-supplied matrix
	 * Un-Normalize: scales back up to device coordinates
	 * Matrix maths requires the normalize/un-normalize in reverse
	 * order.
	 */

	/* back up the user matrix so we can return it on request */
	matrix_from_farray6(&device->abs.usermatrix, calibration);

	/* Un-Normalize */
	matrix_init_translate(&translate,
			      device->abs.absinfo_x->minimum,
			      device->abs.absinfo_y->minimum);
	matrix_init_scale(&scale, sx, sy);
	matrix_mult(&scale, &translate, &scale);

	/* Calibration */
	matrix_mult(&transform, &scale, &transform);

	/* Normalize */
	matrix_init_translate(&translate,
			      -device->abs.absinfo_x->minimum/sx,
			      -device->abs.absinfo_y->minimum/sy);
	matrix_init_scale(&scale, 1.0/sx, 1.0/sy);
	matrix_mult(&scale, &translate, &scale);

	/* store final matrix in device */
	matrix_mult(&device->abs.calibration, &transform, &scale);
}

int
evdev_device_has_capability(struct evdev_device *device,
			    enum libinput_device_capability capability)
{
	switch (capability) {
	case LIBINPUT_DEVICE_CAP_POINTER:
		return !!(device->seat_caps & EVDEV_DEVICE_POINTER);
	case LIBINPUT_DEVICE_CAP_KEYBOARD:
		return !!(device->seat_caps & EVDEV_DEVICE_KEYBOARD);
	case LIBINPUT_DEVICE_CAP_TOUCH:
		return !!(device->seat_caps & EVDEV_DEVICE_TOUCH);
	default:
		return 0;
	}
}

int
evdev_device_get_size(struct evdev_device *device,
		      double *width,
		      double *height)
{
	const struct input_absinfo *x, *y;

	x = libevdev_get_abs_info(device->evdev, ABS_X);
	y = libevdev_get_abs_info(device->evdev, ABS_Y);

	if (!x || !y || device->abs.fake_resolution ||
	    !x->resolution || !y->resolution)
		return -1;

	*width = evdev_convert_to_mm(x, x->maximum);
	*height = evdev_convert_to_mm(y, y->maximum);

	return 0;
}

int
evdev_device_has_button(struct evdev_device *device, uint32_t code)
{
	if (!(device->seat_caps & EVDEV_DEVICE_POINTER))
		return -1;

	return libevdev_has_event_code(device->evdev, EV_KEY, code);
}

static inline bool
evdev_is_scrolling(const struct evdev_device *device,
		   enum libinput_pointer_axis axis)
{
	assert(axis == LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL ||
	       axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);

	return (device->scroll.direction & (1 << axis)) != 0;
}

static inline void
evdev_start_scrolling(struct evdev_device *device,
		      enum libinput_pointer_axis axis)
{
	assert(axis == LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL ||
	       axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);

	device->scroll.direction |= (1 << axis);
}

void
evdev_post_scroll(struct evdev_device *device,
		  uint64_t time,
		  double dx,
		  double dy)
{
	double trigger_horiz, trigger_vert;

	if (!evdev_is_scrolling(device,
				LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
		device->scroll.buildup_vertical += dy;
	if (!evdev_is_scrolling(device,
				LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
		device->scroll.buildup_horizontal += dx;

	trigger_vert = device->scroll.buildup_vertical;
	trigger_horiz = device->scroll.buildup_horizontal;

	/* If we're not scrolling yet, use a distance trigger: moving
	   past a certain distance starts scrolling */
	if (!evdev_is_scrolling(device,
				LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL) &&
	    !evdev_is_scrolling(device,
				LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) {
		if (fabs(trigger_vert) >= device->scroll.threshold)
			evdev_start_scrolling(device,
					      LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
		if (fabs(trigger_horiz) >= device->scroll.threshold)
			evdev_start_scrolling(device,
					      LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
	/* We're already scrolling in one direction. Require some
	   trigger speed to start scrolling in the other direction */
	} else if (!evdev_is_scrolling(device,
			       LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) {
		if (fabs(dy) >= device->scroll.threshold)
			evdev_start_scrolling(device,
				      LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
	} else if (!evdev_is_scrolling(device,
				LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)) {
		if (fabs(dx) >= device->scroll.threshold)
			evdev_start_scrolling(device,
				      LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
	}

	/* We use the trigger to enable, but the delta from this event for
	 * the actual scroll movement. Otherwise we get a jump once
	 * scrolling engages */
	if (dy != 0.0 &&
	    evdev_is_scrolling(device,
			       LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) {
		evdev_notify_axis(device,
				  time,
				  LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL,
				  dy);
	}

	if (dx != 0.0 &&
	    evdev_is_scrolling(device,
			       LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)) {
		evdev_notify_axis(device,
				  time,
				  LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL,
				  dx);
	}
}

void
evdev_stop_scroll(struct evdev_device *device, uint64_t time)
{
	/* terminate scrolling with a zero scroll event */
	if (device->scroll.direction & (1 << LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
		pointer_notify_axis(&device->base,
				    time,
				    LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL,
				    0);
	if (device->scroll.direction & (1 << LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
		pointer_notify_axis(&device->base,
				    time,
				    LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL,
				    0);

	device->scroll.buildup_horizontal = 0;
	device->scroll.buildup_vertical = 0;
	device->scroll.direction = 0;
}

static void
release_pressed_keys(struct evdev_device *device)
{
	struct libinput *libinput = device->base.seat->libinput;
	uint64_t time;
	int code;

	if ((time = libinput_now(libinput)) == 0)
		return;

	for (code = 0; code < KEY_CNT; code++) {
		int count = get_key_down_count(device, code);

		if (count > 1) {
			log_bug_libinput(libinput,
					 "Key %d is down %d times.\n",
					 code,
					 count);
		}

		while (get_key_down_count(device, code) > 0) {
			switch (get_key_type(code)) {
			case EVDEV_KEY_TYPE_NONE:
				break;
			case EVDEV_KEY_TYPE_KEY:
				evdev_keyboard_notify_key(
					device,
					time,
					code,
					LIBINPUT_KEY_STATE_RELEASED);
				break;
			case EVDEV_KEY_TYPE_BUTTON:
				evdev_pointer_notify_button(
					device,
					time,
					evdev_to_left_handed(device, code),
					LIBINPUT_BUTTON_STATE_RELEASED);
				break;
			}
		}
	}
}

void
evdev_notify_suspended_device(struct evdev_device *device)
{
	struct libinput_device *it;

	if (device->suspended)
		return;

	list_for_each(it, &device->base.seat->devices_list, link) {
		struct evdev_device *d = (struct evdev_device*)it;
		if (it == &device->base)
			continue;

		if (d->dispatch->interface->device_suspended)
			d->dispatch->interface->device_suspended(d, device);
	}

	device->suspended = 1;
}

void
evdev_notify_resumed_device(struct evdev_device *device)
{
	struct libinput_device *it;

	if (!device->suspended)
		return;

	list_for_each(it, &device->base.seat->devices_list, link) {
		struct evdev_device *d = (struct evdev_device*)it;
		if (it == &device->base)
			continue;

		if (d->dispatch->interface->device_resumed)
			d->dispatch->interface->device_resumed(d, device);
	}

	device->suspended = 0;
}

int
evdev_device_suspend(struct evdev_device *device)
{
	evdev_notify_suspended_device(device);

	if (device->source) {
		libinput_remove_source(device->base.seat->libinput,
				       device->source);
		device->source = NULL;
	}

	release_pressed_keys(device);

#ifdef __linux__
	if (device->mtdev) {
		mtdev_close_delete(device->mtdev);
		device->mtdev = NULL;
	}
#endif

	if (device->fd != -1) {
		close_restricted(device->base.seat->libinput, device->fd);
		device->fd = -1;
	}

	return 0;
}

int
evdev_device_resume(struct evdev_device *device)
{
	struct libinput *libinput = device->base.seat->libinput;
	int fd;
	const char *devnode;

	if (device->fd != -1)
		return 0;

	if (device->was_removed)
		return -ENODEV;

	devnode = udev_device_get_devnode(device->udev_device);
	fd = open_restricted(libinput, devnode, O_RDWR | O_NONBLOCK);

	if (fd < 0)
		return -errno;

	if (evdev_device_compare_syspath(device->udev_device, fd)) {
		close_restricted(libinput, fd);
		return -ENODEV;
	}

	device->fd = fd;

#ifdef __linux__
	if (evdev_need_mtdev(device)) {
		device->mtdev = mtdev_new_open(device->fd);
		if (!device->mtdev)
			return -ENODEV;
	}
#endif

	device->source =
		libinput_add_fd(libinput, fd, evdev_device_dispatch, device);
	if (!device->source) {
#ifdef __linux__
		mtdev_close_delete(device->mtdev);
#endif
		return -ENOMEM;
	}

	memset(device->hw_key_mask, 0, sizeof(device->hw_key_mask));

	evdev_notify_resumed_device(device);

	return 0;
}

void
evdev_device_remove(struct evdev_device *device)
{
	struct libinput_device *dev;

	list_for_each(dev, &device->base.seat->devices_list, link) {
		struct evdev_device *d = (struct evdev_device*)dev;
		if (dev == &device->base)
			continue;

		if (d->dispatch->interface->device_removed)
			d->dispatch->interface->device_removed(d, device);
	}

	evdev_device_suspend(device);

	if (device->dispatch->interface->remove)
		device->dispatch->interface->remove(device->dispatch);

	/* A device may be removed while suspended, mark it to
	 * skip re-opening a different device with the same node */
	device->was_removed = true;

	list_remove(&device->base.link);

	notify_removed_device(&device->base);
	libinput_device_unref(&device->base);
}

void
evdev_device_destroy(struct evdev_device *device)
{
	struct evdev_dispatch *dispatch;

	dispatch = device->dispatch;
	if (dispatch)
		dispatch->interface->destroy(dispatch);

	filter_destroy(device->pointer.filter);
	libinput_seat_unref(device->base.seat);
	libevdev_free(device->evdev);
	udev_device_unref(device->udev_device);
	free(device->mt.slots);
	free(device);
}
