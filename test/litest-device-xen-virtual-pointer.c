/*
 * Copyright © 2014 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include "litest.h"
#include "litest-int.h"
#include <assert.h>

static void
litest_xen_virtual_pointer_touch_setup(void)
{
	struct litest_device *d = litest_create_device(LITEST_XEN_VIRTUAL_POINTER);
	litest_set_current_device(d);
}

static void touch_down(struct litest_device *d, unsigned int slot,
		       double x, double y)
{
	assert(slot == 0);

	litest_event(d, EV_ABS, ABS_X, litest_scale(d, ABS_X, x));
	litest_event(d, EV_ABS, ABS_Y, litest_scale(d, ABS_Y, y));
	litest_event(d, EV_SYN, SYN_REPORT, 0);
}

static void touch_move(struct litest_device *d, unsigned int slot,
		       double x, double y)
{
	assert(slot == 0);

	litest_event(d, EV_ABS, ABS_X, litest_scale(d, ABS_X, x));
	litest_event(d, EV_ABS, ABS_Y, litest_scale(d, ABS_Y, y));
	litest_event(d, EV_SYN, SYN_REPORT, 0);
}

static void touch_up(struct litest_device *d, unsigned int slot)
{
	assert(slot == 0);
	litest_event(d, EV_SYN, SYN_REPORT, 0);
}

static struct litest_device_interface interface = {
	.touch_down = touch_down,
	.touch_move = touch_move,
	.touch_up = touch_up,
};

static struct input_absinfo absinfo[] = {
	{ ABS_X, 0, 800, 0, 0, 0 },
	{ ABS_Y, 0, 800, 0, 0, 0 },
	{ .value = -1 },
};

static struct input_id input_id = {
	.bustype = 0x01,
	.vendor = 0x5853,
	.product = 0xfffe,
};

static int events[] = {
	EV_KEY, BTN_LEFT,
	EV_KEY, BTN_RIGHT,
	EV_KEY, BTN_MIDDLE,
	EV_KEY, BTN_SIDE,
	EV_KEY, BTN_EXTRA,
	EV_KEY, BTN_FORWARD,
	EV_KEY, BTN_BACK,
	EV_KEY, BTN_TASK,
	EV_REL, REL_WHEEL,
	-1, -1,
};

struct litest_test_device litest_xen_virtual_pointer_device = {
	.type = LITEST_XEN_VIRTUAL_POINTER,
	.features = LITEST_WHEEL | LITEST_BUTTON | LITEST_ABSOLUTE,
	.shortname = "xen pointer",
	.setup = litest_xen_virtual_pointer_touch_setup,
	.interface = &interface,

	.name = "Xen Virtual Pointer",
	.id = &input_id,
	.events = events,
	.absinfo = absinfo,
};
