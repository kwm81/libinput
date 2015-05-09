/*
 * Copyright © 2013 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "evdev-mt-touchpad.h"

#define CASE_RETURN_STRING(a) case a: return #a

#define DEFAULT_TAP_TIMEOUT_PERIOD 180
#define DEFAULT_DRAG_TIMEOUT_PERIOD 500
#define DEFAULT_TAP_MOVE_THRESHOLD TP_MM_TO_DPI_NORMALIZED(3)

enum tap_event {
	TAP_EVENT_TOUCH = 12,
	TAP_EVENT_MOTION,
	TAP_EVENT_RELEASE,
	TAP_EVENT_BUTTON,
	TAP_EVENT_TIMEOUT,
};

/*****************************************
 * DO NOT EDIT THIS FILE!
 *
 * Look at the state diagram in doc/touchpad-tap-state-machine.svg, or
 * online at
 * https://drive.google.com/file/d/0B1NwWmji69noYTdMcU1kTUZuUVE/edit?usp=sharing
 * (it's a http://draw.io diagram)
 *
 * Any changes in this file must be represented in the diagram.
 */

static inline const char*
tap_state_to_str(enum tp_tap_state state)
{
	switch(state) {
	CASE_RETURN_STRING(TAP_STATE_IDLE);
	CASE_RETURN_STRING(TAP_STATE_HOLD);
	CASE_RETURN_STRING(TAP_STATE_TOUCH);
	CASE_RETURN_STRING(TAP_STATE_TAPPED);
	CASE_RETURN_STRING(TAP_STATE_TOUCH_2);
	CASE_RETURN_STRING(TAP_STATE_TOUCH_2_HOLD);
	CASE_RETURN_STRING(TAP_STATE_TOUCH_3);
	CASE_RETURN_STRING(TAP_STATE_TOUCH_3_HOLD);
	CASE_RETURN_STRING(TAP_STATE_DRAGGING);
	CASE_RETURN_STRING(TAP_STATE_DRAGGING_WAIT);
	CASE_RETURN_STRING(TAP_STATE_DRAGGING_OR_DOUBLETAP);
	CASE_RETURN_STRING(TAP_STATE_DRAGGING_2);
	CASE_RETURN_STRING(TAP_STATE_MULTITAP);
	CASE_RETURN_STRING(TAP_STATE_MULTITAP_DOWN);
	CASE_RETURN_STRING(TAP_STATE_DEAD);
	}
	return NULL;
}

static inline const char*
tap_event_to_str(enum tap_event event)
{
	switch(event) {
	CASE_RETURN_STRING(TAP_EVENT_TOUCH);
	CASE_RETURN_STRING(TAP_EVENT_MOTION);
	CASE_RETURN_STRING(TAP_EVENT_RELEASE);
	CASE_RETURN_STRING(TAP_EVENT_TIMEOUT);
	CASE_RETURN_STRING(TAP_EVENT_BUTTON);
	}
	return NULL;
}
#undef CASE_RETURN_STRING

static void
tp_tap_notify(struct tp_dispatch *tp,
	      uint64_t time,
	      int nfingers,
	      enum libinput_button_state state)
{
	int32_t button;

	switch (nfingers) {
	case 1: button = BTN_LEFT; break;
	case 2: button = BTN_RIGHT; break;
	case 3: button = BTN_MIDDLE; break;
	default:
		return;
	}

	if (state == LIBINPUT_BUTTON_STATE_PRESSED)
		tp->tap.buttons_pressed |= (1 << nfingers);
	else
		tp->tap.buttons_pressed &= ~(1 << nfingers);

	evdev_pointer_notify_button(tp->device,
				    time,
				    button,
				    state);
}

static void
tp_tap_set_timer(struct tp_dispatch *tp, uint64_t time)
{
	libinput_timer_set(&tp->tap.timer, time + DEFAULT_TAP_TIMEOUT_PERIOD);
}

static void
tp_tap_set_drag_timer(struct tp_dispatch *tp, uint64_t time)
{
	libinput_timer_set(&tp->tap.timer, time + DEFAULT_DRAG_TIMEOUT_PERIOD);
}

static void
tp_tap_clear_timer(struct tp_dispatch *tp)
{
	libinput_timer_cancel(&tp->tap.timer);
}

static void
tp_tap_idle_handle_event(struct tp_dispatch *tp,
			 struct tp_touch *t,
			 enum tap_event event, uint64_t time)
{
	struct libinput *libinput = tp->device->base.seat->libinput;

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_TOUCH;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_RELEASE:
		break;
	case TAP_EVENT_MOTION:
		log_bug_libinput(libinput,
				 "invalid tap event, no fingers are down\n");
		break;
	case TAP_EVENT_TIMEOUT:
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	}
}

static void
tp_tap_touch_handle_event(struct tp_dispatch *tp,
			  struct tp_touch *t,
			  enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_TOUCH_2;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_TAPPED;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_PRESSED);
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_TIMEOUT:
	case TAP_EVENT_MOTION:
		tp->tap.state = TAP_STATE_HOLD;
		tp_tap_clear_timer(tp);
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	}
}

static void
tp_tap_hold_handle_event(struct tp_dispatch *tp,
			 struct tp_touch *t,
			 enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_TOUCH_2;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_IDLE;
		break;
	case TAP_EVENT_MOTION:
	case TAP_EVENT_TIMEOUT:
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	}
}

static void
tp_tap_tapped_handle_event(struct tp_dispatch *tp,
			   struct tp_touch *t,
			   enum tap_event event, uint64_t time)
{
	struct libinput *libinput = tp->device->base.seat->libinput;

	switch (event) {
	case TAP_EVENT_MOTION:
	case TAP_EVENT_RELEASE:
		log_bug_libinput(libinput,
				 "invalid tap event when fingers are up\n");
		break;
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DRAGGING_OR_DOUBLETAP;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_IDLE;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	}
}

static void
tp_tap_touch2_handle_event(struct tp_dispatch *tp,
			   struct tp_touch *t,
			   enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_TOUCH_3;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_HOLD;
		if (t->tap.state == TAP_TOUCH_STATE_TOUCH) {
			tp_tap_notify(tp, time, 2, LIBINPUT_BUTTON_STATE_PRESSED);
			tp_tap_notify(tp, time, 2, LIBINPUT_BUTTON_STATE_RELEASED);
		}
		tp_tap_clear_timer(tp);
		break;
	case TAP_EVENT_MOTION:
		tp_tap_clear_timer(tp);
		/* fallthrough */
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_TOUCH_2_HOLD;
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	}
}

static void
tp_tap_touch2_hold_handle_event(struct tp_dispatch *tp,
				struct tp_touch *t,
				enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_TOUCH_3;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_HOLD;
		break;
	case TAP_EVENT_MOTION:
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_TOUCH_2_HOLD;
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	}
}

static void
tp_tap_touch3_handle_event(struct tp_dispatch *tp,
			   struct tp_touch *t,
			   enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_clear_timer(tp);
		break;
	case TAP_EVENT_MOTION:
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_TOUCH_3_HOLD;
		tp_tap_clear_timer(tp);
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_TOUCH_2_HOLD;
		if (t->tap.state == TAP_TOUCH_STATE_TOUCH) {
			tp_tap_notify(tp, time, 3, LIBINPUT_BUTTON_STATE_PRESSED);
			tp_tap_notify(tp, time, 3, LIBINPUT_BUTTON_STATE_RELEASED);
		}
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	}
}

static void
tp_tap_touch3_hold_handle_event(struct tp_dispatch *tp,
				struct tp_touch *t,
				enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_TOUCH_2_HOLD;
		break;
	case TAP_EVENT_MOTION:
	case TAP_EVENT_TIMEOUT:
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	}
}

static void
tp_tap_dragging_or_doubletap_handle_event(struct tp_dispatch *tp,
					  struct tp_touch *t,
					  enum tap_event event, uint64_t time)
{
	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DRAGGING_2;
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_MULTITAP;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_PRESSED);
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_MOTION:
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_DRAGGING;
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	}
}

static void
tp_tap_dragging_handle_event(struct tp_dispatch *tp,
			     struct tp_touch *t,
			     enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DRAGGING_2;
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_DRAGGING_WAIT;
		tp_tap_set_drag_timer(tp, time);
		break;
	case TAP_EVENT_MOTION:
	case TAP_EVENT_TIMEOUT:
		/* noop */
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	}
}

static void
tp_tap_dragging_wait_handle_event(struct tp_dispatch *tp,
				  struct tp_touch *t,
				  enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DRAGGING;
		tp_tap_clear_timer(tp);
		break;
	case TAP_EVENT_RELEASE:
	case TAP_EVENT_MOTION:
		break;
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_IDLE;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	}
}

static void
tp_tap_dragging2_handle_event(struct tp_dispatch *tp,
			      struct tp_touch *t,
			      enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_DRAGGING;
		break;
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_MOTION:
	case TAP_EVENT_TIMEOUT:
		/* noop */
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	}
}

static void
tp_tap_multitap_handle_event(struct tp_dispatch *tp,
			      struct tp_touch *t,
			      enum tap_event event, uint64_t time)
{
	struct libinput *libinput = tp->device->base.seat->libinput;

	switch (event) {
	case TAP_EVENT_RELEASE:
		log_bug_libinput(libinput,
				 "invalid tap event, no fingers are down\n");
		break;
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_MULTITAP_DOWN;
		tp->tap.multitap_last_time = time;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_PRESSED);
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_MOTION:
		log_bug_libinput(libinput,
				 "invalid tap event, no fingers are down\n");
		break;
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_IDLE;
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_IDLE;
		tp_tap_clear_timer(tp);
		break;
	}
}

static void
tp_tap_multitap_down_handle_event(struct tp_dispatch *tp,
				  struct tp_touch *t,
				  enum tap_event event,
				  uint64_t time)
{
	switch (event) {
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_MULTITAP;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DRAGGING_2;
		tp_tap_clear_timer(tp);
		break;
	case TAP_EVENT_MOTION:
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_DRAGGING;
		tp_tap_clear_timer(tp);
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		tp_tap_clear_timer(tp);
		break;
	}
}

static void
tp_tap_dead_handle_event(struct tp_dispatch *tp,
			 struct tp_touch *t,
			 enum tap_event event,
			 uint64_t time)
{

	switch (event) {
	case TAP_EVENT_RELEASE:
		if (tp->nfingers_down == 0)
			tp->tap.state = TAP_STATE_IDLE;
		break;
	case TAP_EVENT_TOUCH:
	case TAP_EVENT_MOTION:
	case TAP_EVENT_TIMEOUT:
	case TAP_EVENT_BUTTON:
		break;
	}
}

static void
tp_tap_handle_event(struct tp_dispatch *tp,
		    struct tp_touch *t,
		    enum tap_event event,
		    uint64_t time)
{
	struct libinput *libinput = tp->device->base.seat->libinput;
	enum tp_tap_state current;

	current = tp->tap.state;

	switch(tp->tap.state) {
	case TAP_STATE_IDLE:
		tp_tap_idle_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_TOUCH:
		tp_tap_touch_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_HOLD:
		tp_tap_hold_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_TAPPED:
		tp_tap_tapped_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_TOUCH_2:
		tp_tap_touch2_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_TOUCH_2_HOLD:
		tp_tap_touch2_hold_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_TOUCH_3:
		tp_tap_touch3_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_TOUCH_3_HOLD:
		tp_tap_touch3_hold_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_DRAGGING_OR_DOUBLETAP:
		tp_tap_dragging_or_doubletap_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_DRAGGING:
		tp_tap_dragging_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_DRAGGING_WAIT:
		tp_tap_dragging_wait_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_DRAGGING_2:
		tp_tap_dragging2_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_MULTITAP:
		tp_tap_multitap_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_MULTITAP_DOWN:
		tp_tap_multitap_down_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_DEAD:
		tp_tap_dead_handle_event(tp, t, event, time);
		break;
	}

	if (tp->tap.state == TAP_STATE_IDLE || tp->tap.state == TAP_STATE_DEAD)
		tp_tap_clear_timer(tp);

	log_debug(libinput,
		  "tap state: %s → %s → %s\n",
		  tap_state_to_str(current),
		  tap_event_to_str(event),
		  tap_state_to_str(tp->tap.state));
}

static bool
tp_tap_exceeds_motion_threshold(struct tp_dispatch *tp,
				struct tp_touch *t)
{
	struct normalized_coords norm =
		tp_normalize_delta(tp, device_delta(t->point,
						    t->tap.initial));

	return normalized_length(norm) > DEFAULT_TAP_MOVE_THRESHOLD;
}

static bool
tp_tap_enabled(struct tp_dispatch *tp)
{
	return tp->tap.enabled && !tp->tap.suspended;
}

int
tp_tap_handle_state(struct tp_dispatch *tp, uint64_t time)
{
	struct tp_touch *t;
	int filter_motion = 0;

	if (!tp_tap_enabled(tp))
		return 0;

	/* Handle queued button pressed events from clickpads. For touchpads
	 * with separate physical buttons, ignore button pressed events so they
	 * don't interfere with tapping. */
	if (tp->buttons.is_clickpad && tp->queued & TOUCHPAD_EVENT_BUTTON_PRESS)
		tp_tap_handle_event(tp, NULL, TAP_EVENT_BUTTON, time);

	tp_for_each_touch(tp, t) {
		if (!t->dirty || t->state == TOUCH_NONE)
			continue;

		if (tp->buttons.is_clickpad &&
		    tp->queued & TOUCHPAD_EVENT_BUTTON_PRESS)
			t->tap.state = TAP_TOUCH_STATE_DEAD;

		if (t->state == TOUCH_BEGIN) {
			t->tap.state = TAP_TOUCH_STATE_TOUCH;
			t->tap.initial = t->point;
			tp_tap_handle_event(tp, t, TAP_EVENT_TOUCH, time);

			/* If we think this is a palm, pretend there's a
			 * motion event which will prevent tap clicks
			 * without requiring extra states in the FSM.
			 */
			if (tp_palm_tap_is_palm(tp, t))
				tp_tap_handle_event(tp, t, TAP_EVENT_MOTION, time);

		} else if (t->state == TOUCH_END) {
			tp_tap_handle_event(tp, t, TAP_EVENT_RELEASE, time);
			t->tap.state = TAP_TOUCH_STATE_IDLE;
		} else if (tp->tap.state != TAP_STATE_IDLE &&
			   tp_tap_exceeds_motion_threshold(tp, t)) {
			struct tp_touch *tmp;

			/* Any touch exceeding the threshold turns all
			 * touches into DEAD */
			tp_for_each_touch(tp, tmp) {
				if (tmp->tap.state == TAP_TOUCH_STATE_TOUCH)
					tmp->tap.state = TAP_TOUCH_STATE_DEAD;
			}

			tp_tap_handle_event(tp, t, TAP_EVENT_MOTION, time);
		}
	}

	/**
	 * In any state where motion exceeding the move threshold would
	 * move to the next state, filter that motion until we actually
	 * exceed it. This prevents small motion events while we're waiting
	 * on a decision if a tap is a tap.
	 */
	switch (tp->tap.state) {
	case TAP_STATE_TOUCH:
	case TAP_STATE_TAPPED:
	case TAP_STATE_DRAGGING_OR_DOUBLETAP:
	case TAP_STATE_TOUCH_2:
	case TAP_STATE_TOUCH_3:
	case TAP_STATE_MULTITAP_DOWN:
		filter_motion = 1;
		break;

	default:
		break;

	}

	return filter_motion;
}

static void
tp_tap_handle_timeout(uint64_t time, void *data)
{
	struct tp_dispatch *tp = data;
	struct tp_touch *t;

	tp_tap_handle_event(tp, NULL, TAP_EVENT_TIMEOUT, time);

	tp_for_each_touch(tp, t) {
		if (t->state == TOUCH_NONE ||
		    t->tap.state == TAP_TOUCH_STATE_IDLE)
			continue;

		t->tap.state = TAP_TOUCH_STATE_DEAD;
	}
}

static void
tp_tap_enabled_update(struct tp_dispatch *tp, bool suspended, bool enabled, uint64_t time)
{
	bool was_enabled = tp_tap_enabled(tp);

	tp->tap.suspended = suspended;
	tp->tap.enabled = enabled;

	if (tp_tap_enabled(tp) == was_enabled)
		return;

	if (tp_tap_enabled(tp)) {
		/* Must restart in DEAD if fingers are down atm */
		tp->tap.state =
			tp->nfingers_down ? TAP_STATE_DEAD : TAP_STATE_IDLE;
	} else {
		tp_release_all_taps(tp, time);
	}
}

static int
tp_tap_config_count(struct libinput_device *device)
{
	struct evdev_dispatch *dispatch;
	struct tp_dispatch *tp = NULL;

	dispatch = ((struct evdev_device *) device)->dispatch;
	tp = container_of(dispatch, tp, base);

	return min(tp->ntouches, 3); /* we only do up to 3 finger tap */
}

static enum libinput_config_status
tp_tap_config_set_enabled(struct libinput_device *device,
			  enum libinput_config_tap_state enabled)
{
	struct evdev_dispatch *dispatch = ((struct evdev_device *) device)->dispatch;
	struct tp_dispatch *tp = NULL;

	tp = container_of(dispatch, tp, base);
	tp_tap_enabled_update(tp, tp->tap.suspended,
			      (enabled == LIBINPUT_CONFIG_TAP_ENABLED),
			      libinput_now(device->seat->libinput));

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static enum libinput_config_tap_state
tp_tap_config_is_enabled(struct libinput_device *device)
{
	struct evdev_dispatch *dispatch;
	struct tp_dispatch *tp = NULL;

	dispatch = ((struct evdev_device *) device)->dispatch;
	tp = container_of(dispatch, tp, base);

	return tp->tap.enabled ? LIBINPUT_CONFIG_TAP_ENABLED :
				 LIBINPUT_CONFIG_TAP_DISABLED;
}

static enum libinput_config_tap_state
tp_tap_default(struct evdev_device *evdev)
{
	/**
	 * If we don't have a left button we must have tapping enabled by
	 * default.
	 */
	if (!libevdev_has_event_code(evdev->evdev, EV_KEY, BTN_LEFT))
		return LIBINPUT_CONFIG_TAP_ENABLED;

	/**
	 * Tapping is disabled by default for two reasons:
	 * * if you don't know that tapping is a thing (or enabled by
	 *   default), you get spurious mouse events that make the desktop
	 *   feel buggy.
	 * * if you do know what tapping is and you want it, you
	 *   usually know where to enable it, or at least you can search for
	 *   it.
	 */
	return LIBINPUT_CONFIG_TAP_DISABLED;
}

static enum libinput_config_tap_state
tp_tap_config_get_default(struct libinput_device *device)
{
	struct evdev_device *evdev = (struct evdev_device *)device;

	return tp_tap_default(evdev);
}

int
tp_init_tap(struct tp_dispatch *tp)
{
	tp->tap.config.count = tp_tap_config_count;
	tp->tap.config.set_enabled = tp_tap_config_set_enabled;
	tp->tap.config.get_enabled = tp_tap_config_is_enabled;
	tp->tap.config.get_default = tp_tap_config_get_default;
	tp->device->base.config.tap = &tp->tap.config;

	tp->tap.state = TAP_STATE_IDLE;
	tp->tap.enabled = tp_tap_default(tp->device);

	libinput_timer_init(&tp->tap.timer,
			    tp->device->base.seat->libinput,
			    tp_tap_handle_timeout, tp);

	return 0;
}

void
tp_remove_tap(struct tp_dispatch *tp)
{
	libinput_timer_cancel(&tp->tap.timer);
}

void
tp_release_all_taps(struct tp_dispatch *tp, uint64_t now)
{
	int i;

	for (i = 1; i <= 3; i++) {
		if (tp->tap.buttons_pressed & (1 << i))
			tp_tap_notify(tp, now, i, LIBINPUT_BUTTON_STATE_RELEASED);
	}

	tp->tap.state = tp->nfingers_down ? TAP_STATE_DEAD : TAP_STATE_IDLE;
}

void
tp_tap_suspend(struct tp_dispatch *tp, uint64_t time)
{
	tp_tap_enabled_update(tp, true, tp->tap.enabled, time);
}

void
tp_tap_resume(struct tp_dispatch *tp, uint64_t time)
{
	tp_tap_enabled_update(tp, false, tp->tap.enabled, time);
}

bool
tp_tap_dragging(struct tp_dispatch *tp)
{
	switch (tp->tap.state) {
	case TAP_STATE_DRAGGING:
	case TAP_STATE_DRAGGING_2:
	case TAP_STATE_DRAGGING_WAIT:
		return true;
	default:
		return false;
	}
}
