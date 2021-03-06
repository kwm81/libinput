/*
 * Copyright © 2014 Jonas Ådahl <jadahl@gmail.com>
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

#include <check.h>
#include <stdio.h>

#include "libinput-util.h"
#include "litest.h"

START_TEST(keyboard_seat_key_count)
{
	const int num_devices = 4;
	struct litest_device *devices[num_devices];
	struct libinput *libinput;
	struct libinput_event *ev;
	struct libinput_event_keyboard *kev;
	int i;
	int seat_key_count = 0;
	int expected_key_button_count = 0;
	char device_name[255];

	libinput = litest_create_context();
	for (i = 0; i < num_devices; ++i) {
		sprintf(device_name, "litest Generic keyboard (%d)", i);
		devices[i] = litest_add_device_with_overrides(libinput,
							      LITEST_KEYBOARD,
							      device_name,
							      NULL, NULL, NULL);
	}

	for (i = 0; i < num_devices; ++i)
		litest_keyboard_key(devices[i], KEY_A, true);

	libinput_dispatch(libinput);
	while ((ev = libinput_get_event(libinput))) {
		if (libinput_event_get_type(ev) !=
		    LIBINPUT_EVENT_KEYBOARD_KEY) {
			libinput_event_destroy(ev);
			libinput_dispatch(libinput);
			continue;
		}

		kev = litest_is_keyboard_event(ev,
					       KEY_A,
					       LIBINPUT_KEY_STATE_PRESSED);

		++expected_key_button_count;
		seat_key_count =
			libinput_event_keyboard_get_seat_key_count(kev);
		ck_assert_int_eq(expected_key_button_count, seat_key_count);

		libinput_event_destroy(ev);
		libinput_dispatch(libinput);
	}

	ck_assert_int_eq(seat_key_count, num_devices);

	for (i = 0; i < num_devices; ++i)
		litest_keyboard_key(devices[i], KEY_A, false);

	libinput_dispatch(libinput);
	while ((ev = libinput_get_event(libinput))) {
		if (libinput_event_get_type(ev) !=
		    LIBINPUT_EVENT_KEYBOARD_KEY) {
			libinput_event_destroy(ev);
			libinput_dispatch(libinput);
			continue;
		}

		kev = libinput_event_get_keyboard_event(ev);
		ck_assert_notnull(kev);
		ck_assert_int_eq(libinput_event_keyboard_get_key(kev), KEY_A);
		ck_assert_int_eq(libinput_event_keyboard_get_key_state(kev),
				 LIBINPUT_KEY_STATE_RELEASED);

		--expected_key_button_count;
		seat_key_count =
			libinput_event_keyboard_get_seat_key_count(kev);
		ck_assert_int_eq(expected_key_button_count, seat_key_count);

		libinput_event_destroy(ev);
		libinput_dispatch(libinput);
	}

	ck_assert_int_eq(seat_key_count, 0);

	for (i = 0; i < num_devices; ++i)
		litest_delete_device(devices[i]);
	libinput_unref(libinput);
}
END_TEST

START_TEST(keyboard_ignore_no_pressed_release)
{
	struct litest_device *dev;
	struct libinput *unused_libinput;
	struct libinput *libinput;
	struct libinput_event *event;
	struct libinput_event_keyboard *kevent;
	int events[] = {
		EV_KEY, KEY_A,
		-1, -1,
	};
	enum libinput_key_state *state;
	enum libinput_key_state expected_states[] = {
		LIBINPUT_KEY_STATE_PRESSED,
		LIBINPUT_KEY_STATE_RELEASED,
	};

	/* We can't send pressed -> released -> pressed events using uinput
	 * as such non-symmetric events are dropped. Work-around this by first
	 * adding the test device to the tested context after having sent an
	 * initial pressed event. */
	unused_libinput = litest_create_context();
	dev = litest_add_device_with_overrides(unused_libinput,
					       LITEST_KEYBOARD,
					       "Generic keyboard",
					       NULL, NULL, events);

	litest_keyboard_key(dev, KEY_A, true);
	litest_drain_events(unused_libinput);

	libinput = litest_create_context();
	libinput_path_add_device(libinput,
				 libevdev_uinput_get_devnode(dev->uinput));
	litest_drain_events(libinput);

	litest_keyboard_key(dev, KEY_A, false);
	litest_keyboard_key(dev, KEY_A, true);
	litest_keyboard_key(dev, KEY_A, false);

	libinput_dispatch(libinput);

	ARRAY_FOR_EACH(expected_states, state) {
		event = libinput_get_event(libinput);
		ck_assert_notnull(event);
		ck_assert_int_eq(libinput_event_get_type(event),
				 LIBINPUT_EVENT_KEYBOARD_KEY);
		kevent = libinput_event_get_keyboard_event(event);
		ck_assert_int_eq(libinput_event_keyboard_get_key(kevent),
				 KEY_A);
		ck_assert_int_eq(libinput_event_keyboard_get_key_state(kevent),
				 *state);
		libinput_event_destroy(event);
		libinput_dispatch(libinput);
	}

	litest_assert_empty_queue(libinput);
	litest_delete_device(dev);
	libinput_unref(libinput);
	libinput_unref(unused_libinput);
}
END_TEST

START_TEST(keyboard_key_auto_release)
{
	struct libinput *libinput;
	struct litest_device *dev;
	struct libinput_event *event;
	enum libinput_event_type type;
	struct libinput_event_keyboard *kevent;
	struct {
		int code;
		int released;
	} keys[] = {
		{ .code = KEY_A, },
		{ .code = KEY_S, },
		{ .code = KEY_D, },
		{ .code = KEY_G, },
		{ .code = KEY_Z, },
		{ .code = KEY_DELETE, },
		{ .code = KEY_F24, },
	};
	int events[2 * (ARRAY_LENGTH(keys) + 1)];
	unsigned i;
	int key;
	int valid_code;

	/* Enable all tested keys on the device */
	i = 0;
	while (i < 2 * ARRAY_LENGTH(keys)) {
		key = keys[i / 2].code;
		events[i++] = EV_KEY;
		events[i++] = key;
	}
	events[i++] = -1;
	events[i++] = -1;

	libinput = litest_create_context();
	dev = litest_add_device_with_overrides(libinput,
					       LITEST_KEYBOARD,
					       "Generic keyboard",
					       NULL, NULL, events);

	litest_drain_events(libinput);

	/* Send pressed events, without releasing */
	for (i = 0; i < ARRAY_LENGTH(keys); ++i) {
		key = keys[i].code;
		litest_event(dev, EV_KEY, key, 1);
		litest_event(dev, EV_SYN, SYN_REPORT, 0);

		libinput_dispatch(libinput);

		event = libinput_get_event(libinput);
		kevent = litest_is_keyboard_event(event,
						  key,
						  LIBINPUT_KEY_STATE_PRESSED);
		libinput_event_destroy(event);
	}

	litest_drain_events(libinput);

	/* "Disconnect" device */
	litest_delete_device(dev);

	/* Mark all released keys until device is removed */
	while (1) {
		event = libinput_get_event(libinput);
		ck_assert_notnull(event);
		type = libinput_event_get_type(event);

		if (type == LIBINPUT_EVENT_DEVICE_REMOVED) {
			libinput_event_destroy(event);
			break;
		}

		ck_assert_int_eq(type, LIBINPUT_EVENT_KEYBOARD_KEY);
		kevent = libinput_event_get_keyboard_event(event);
		ck_assert_int_eq(libinput_event_keyboard_get_key_state(kevent),
				 LIBINPUT_KEY_STATE_RELEASED);
		key = libinput_event_keyboard_get_key(kevent);

		valid_code = 0;
		for (i = 0; i < ARRAY_LENGTH(keys); ++i) {
			if (keys[i].code == key) {
				ck_assert_int_eq(keys[i].released, 0);
				keys[i].released = 1;
				valid_code = 1;
			}
		}
		ck_assert_int_eq(valid_code, 1);
		libinput_event_destroy(event);
	}

	/* Check that all pressed keys has been released. */
	for (i = 0; i < ARRAY_LENGTH(keys); ++i) {
		ck_assert_int_eq(keys[i].released, 1);
	}

	libinput_unref(libinput);
}
END_TEST

START_TEST(keyboard_has_key)
{
	struct litest_device *dev = litest_current_device();
	struct libinput_device *device = dev->libinput_device;
	unsigned int code;
	int evdev_has, libinput_has;

	ck_assert(libinput_device_has_capability(
					 device,
					 LIBINPUT_DEVICE_CAP_KEYBOARD));

	for (code = 0; code < KEY_CNT; code++) {
		evdev_has = libevdev_has_event_code(dev->evdev, EV_KEY, code);
		libinput_has = libinput_device_keyboard_has_key(device, code);
		ck_assert_int_eq(evdev_has, libinput_has);
	}
}
END_TEST

START_TEST(keyboard_keys_bad_device)
{
	struct litest_device *dev = litest_current_device();
	struct libinput_device *device = dev->libinput_device;
	unsigned int code;
	int has_key;

	if (libinput_device_has_capability(device,
					   LIBINPUT_DEVICE_CAP_KEYBOARD))
		return;

	for (code = 0; code < KEY_CNT; code++) {
		has_key = libinput_device_keyboard_has_key(device, code);
		ck_assert_int_eq(has_key, -1);
	}
}
END_TEST

void
litest_setup_tests(void)
{
	litest_add_no_device("keyboard:seat key count", keyboard_seat_key_count);
	litest_add_no_device("keyboard:key counting", keyboard_ignore_no_pressed_release);
	litest_add_no_device("keyboard:key counting", keyboard_key_auto_release);
	litest_add("keyboard:keys", keyboard_has_key, LITEST_KEYS, LITEST_ANY);
	litest_add("keyboard:keys", keyboard_keys_bad_device, LITEST_ANY, LITEST_ANY);
}
