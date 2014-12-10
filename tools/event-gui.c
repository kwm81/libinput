/*
 * Copyright © 2014 Red Hat, Inc.
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
#define _GNU_SOURCE
#include <config.h>

#include <linux/input.h>

#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <glib.h>

#include <libinput.h>
#include <libinput-util.h>

#define clip(val_, min_, max_) min((max_), max((min_), (val_)))

struct touch {
	int active;
	int x, y;
};

struct window {
	GtkWidget *win;
	GtkWidget *area;
	int width, height; /* of window */

	/* sprite position */
	double x, y;

	/* abs position */
	int absx, absy;

	/* scroll bar positions */
	int vx, vy;
	int hx, hy;

	/* touch positions */
	struct touch touches[32];

	/* l/m/r mouse buttons */
	int l, m, r;

	struct libinput_device *devices[50];
};

static int
error(const char *fmt, ...)
{
	va_list args;
	fprintf(stderr, "error: ");

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	return EXIT_FAILURE;
}

static void
msg(const char *fmt, ...)
{
	va_list args;
	printf("info: ");

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}

static void
usage(void)
{
	printf("[path/to/device]\n");
}

static gboolean
draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	struct window *w = data;
	struct touch *t;

	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_rectangle(cr, 0, 0, w->width, w->height);
	cairo_fill(cr);

	/* draw pointer sprite */
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_save(cr);
	cairo_move_to(cr, w->x, w->y);
	cairo_rel_line_to(cr, 10, 15);
	cairo_rel_line_to(cr, -10, 0);
	cairo_rel_line_to(cr, 0, -15);
	cairo_fill(cr);
	cairo_restore(cr);

	/* draw scroll bars */
	cairo_set_source_rgb(cr, .4, .8, 0);

	cairo_save(cr);
	cairo_rectangle(cr, w->vx - 10, w->vy - 20, 20, 40);
	cairo_rectangle(cr, w->hx - 20, w->hy - 10, 40, 20);
	cairo_fill(cr);
	cairo_restore(cr);

	/* touch points */
	cairo_set_source_rgb(cr, .8, .2, .2);

	ARRAY_FOR_EACH(w->touches, t) {
		cairo_save(cr);
		cairo_arc(cr, t->x, t->y, 10, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_restore(cr);
	}

	/* abs position */
	cairo_set_source_rgb(cr, .2, .4, .8);

	cairo_save(cr);
	cairo_arc(cr, w->absx, w->absy, 10, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_restore(cr);

	/* lmr buttons */
	cairo_save(cr);
	if (w->l || w->m || w->r) {
		cairo_set_source_rgb(cr, .2, .8, .8);
		if (w->l)
			cairo_rectangle(cr, w->width/2 - 100, w->height - 200, 70, 30);
		if (w->m)
			cairo_rectangle(cr, w->width/2 - 20, w->height - 200, 40, 30);
		if (w->r)
			cairo_rectangle(cr, w->width/2 + 30, w->height - 200, 70, 30);
		cairo_fill(cr);
	}

	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_rectangle(cr, w->width/2 - 100, w->height - 200, 70, 30);
	cairo_rectangle(cr, w->width/2 - 20, w->height - 200, 40, 30);
	cairo_rectangle(cr, w->width/2 + 30, w->height - 200, 70, 30);
	cairo_stroke(cr);
	cairo_restore(cr);

	return TRUE;
}

static void
map_event_cb(GtkWidget *widget, GdkEvent *event, gpointer data)
{
	struct window *w = data;

	gtk_window_get_size(GTK_WINDOW(widget), &w->width, &w->height);

	w->x = w->width/2;
	w->y = w->height/2;

	w->vx = w->width/2;
	w->vy = w->height/2;
	w->hx = w->width/2;
	w->hy = w->height/2;

	g_signal_connect(G_OBJECT(w->area), "draw", G_CALLBACK(draw), w);

	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      gdk_cursor_new(GDK_BLANK_CURSOR));
}

static void
window_init(struct window *w)
{
	memset(w, 0, sizeof(*w));

	w->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_widget_set_events(w->win, 0);
	gtk_window_set_title(GTK_WINDOW(w->win), "libinput debugging tool");
	gtk_window_set_default_size(GTK_WINDOW(w->win), 1024, 768);
	gtk_window_maximize(GTK_WINDOW(w->win));
	gtk_window_set_resizable(GTK_WINDOW(w->win), TRUE);
	gtk_widget_realize(w->win);
	g_signal_connect(G_OBJECT(w->win), "map-event", G_CALLBACK(map_event_cb), w);
	g_signal_connect(G_OBJECT(w->win), "delete-event", G_CALLBACK(gtk_main_quit), NULL);

	w->area = gtk_drawing_area_new();
	gtk_widget_set_events(w->area, 0);
	gtk_container_add(GTK_CONTAINER(w->win), w->area);
	gtk_widget_show_all(w->win);
}

static void
window_cleanup(struct window *w)
{
	struct libinput_device **dev;
	ARRAY_FOR_EACH(w->devices, dev) {
		if (*dev)
			libinput_device_unref(*dev);
	}
}

static void
change_ptraccel(struct window *w, double amount)
{
	struct libinput_device **dev;

	ARRAY_FOR_EACH(w->devices, dev) {
		double speed;
		enum libinput_config_status status;

		if (*dev == NULL)
			continue;

		if (!libinput_device_config_accel_is_available(*dev))
			continue;

		speed = libinput_device_config_accel_get_speed(*dev);
		speed = clip(speed + amount, -1, 1);

		status = libinput_device_config_accel_set_speed(*dev, speed);

		if (status != LIBINPUT_CONFIG_STATUS_SUCCESS) {
			msg("%s: failed to change accel to %.2f (%s)\n",
			    libinput_device_get_name(*dev),
			    speed,
			    libinput_config_status_to_str(status));
		} else {
			printf("%s: speed is %.2f\n",
			       libinput_device_get_name(*dev),
			       speed);
		}

	}
}


static void
handle_event_device_notify(struct libinput_event *ev)
{
	struct libinput_device *dev = libinput_event_get_device(ev);
	struct libinput *li;
	struct window *w;
	const char *type;
	int i;

	if (libinput_event_get_type(ev) == LIBINPUT_EVENT_DEVICE_ADDED)
		type = "added";
	else
		type = "removed";

	msg("%s %-30s %s\n",
	    libinput_device_get_sysname(dev),
	    libinput_device_get_name(dev),
	    type);

	if (libinput_device_config_tap_get_finger_count(dev) > 0) {
		enum libinput_config_status status;
		status = libinput_device_config_tap_set_enabled(dev,
								LIBINPUT_CONFIG_TAP_ENABLED);
		if (status != LIBINPUT_CONFIG_STATUS_SUCCESS)
			error("%s: Failed to enable tapping\n",
			      libinput_device_get_sysname(dev));
	}

	li = libinput_event_get_context(ev);
	w = libinput_get_user_data(li);

	if (libinput_event_get_type(ev) == LIBINPUT_EVENT_DEVICE_ADDED) {
		for (i = 0; i < ARRAY_LENGTH(w->devices); i++) {
			if (w->devices[i] == NULL) {
				w->devices[i] = libinput_device_ref(dev);
				break;
			}
		}
	} else  {
		for (i = 0; i < ARRAY_LENGTH(w->devices); i++) {
			if (w->devices[i] == dev) {
				libinput_device_unref(w->devices[i]);
				w->devices[i] = NULL;
				break;
			}
		}
	}
}

static void
handle_event_motion(struct libinput_event *ev, struct window *w)
{
	struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
	double dx = libinput_event_pointer_get_dx(p),
	       dy = libinput_event_pointer_get_dy(p);

	w->x += dx;
	w->y += dy;
	w->x = clip(w->x, 0.0, w->width);
	w->y = clip(w->y, 0.0, w->height);
}

static void
handle_event_absmotion(struct libinput_event *ev, struct window *w)
{
	struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
	double x = libinput_event_pointer_get_absolute_x_transformed(p, w->width),
	       y = libinput_event_pointer_get_absolute_y_transformed(p, w->height);

	w->absx = x;
	w->absy = y;
}

static void
handle_event_touch(struct libinput_event *ev, struct window *w)
{
	struct libinput_event_touch *t = libinput_event_get_touch_event(ev);
	int slot = libinput_event_touch_get_seat_slot(t);
	struct touch *touch;
	double x, y;

	if (slot == -1 || slot >= (int) ARRAY_LENGTH(w->touches))
		return;

	touch = &w->touches[slot];

	if (libinput_event_get_type(ev) == LIBINPUT_EVENT_TOUCH_UP) {
		touch->active = 0;
		return;
	}

	x = libinput_event_touch_get_x_transformed(t, w->width),
	y = libinput_event_touch_get_y_transformed(t, w->height);

	touch->active = 1;
	touch->x = (int)x;
	touch->y = (int)y;
}

static void
handle_event_axis(struct libinput_event *ev, struct window *w)
{
	struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
	enum libinput_pointer_axis axis = libinput_event_pointer_get_axis(p);
	double v = libinput_event_pointer_get_axis_value(p);

	switch (axis) {
	case LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL:
		w->vy += (int)v;
		w->vy = clip(w->vy, 0, w->height);
		break;
	case LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL:
		w->hx += (int)v;
		w->hx = clip(w->hx, 0, w->width);
		break;
	default:
		abort();
	}
}

static int
handle_event_keyboard(struct libinput_event *ev, struct window *w)
{
	struct libinput_event_keyboard *k = libinput_event_get_keyboard_event(ev);
	unsigned int key = libinput_event_keyboard_get_key(k);

	if (libinput_event_keyboard_get_key_state(k) ==
	    LIBINPUT_KEY_STATE_RELEASED)
		return 0;

	switch(key) {
	case KEY_ESC:
		return 1;
	case KEY_UP:
		change_ptraccel(w, 0.1);
		break;
	case KEY_DOWN:
		change_ptraccel(w, -0.1);
		break;
	default:
		break;
	}

	return 0;
}

static void
handle_event_button(struct libinput_event *ev, struct window *w)
{
	struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
	unsigned int button = libinput_event_pointer_get_button(p);
	int is_press;

	is_press = libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED;

	switch (button) {
	case BTN_LEFT:
		w->l = is_press;
		break;
	case BTN_RIGHT:
		w->r = is_press;
		break;
	case BTN_MIDDLE:
		w->m = is_press;
		break;
	}

}

static gboolean
handle_event_libinput(GIOChannel *source, GIOCondition condition, gpointer data)
{
	struct libinput *li = data;
	struct window *w = libinput_get_user_data(li);
	struct libinput_event *ev;

	libinput_dispatch(li);

	while ((ev = libinput_get_event(li))) {
		switch (libinput_event_get_type(ev)) {
		case LIBINPUT_EVENT_NONE:
			abort();
		case LIBINPUT_EVENT_DEVICE_ADDED:
		case LIBINPUT_EVENT_DEVICE_REMOVED:
			handle_event_device_notify(ev);
			break;
		case LIBINPUT_EVENT_POINTER_MOTION:
			handle_event_motion(ev, w);
			break;
		case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
			handle_event_absmotion(ev, w);
			break;
		case LIBINPUT_EVENT_TOUCH_DOWN:
		case LIBINPUT_EVENT_TOUCH_MOTION:
		case LIBINPUT_EVENT_TOUCH_UP:
			handle_event_touch(ev, w);
			break;
		case LIBINPUT_EVENT_POINTER_AXIS:
			handle_event_axis(ev, w);
			break;
		case LIBINPUT_EVENT_TOUCH_CANCEL:
		case LIBINPUT_EVENT_TOUCH_FRAME:
			break;
		case LIBINPUT_EVENT_POINTER_BUTTON:
			handle_event_button(ev, w);
			break;
		case LIBINPUT_EVENT_KEYBOARD_KEY:
			if (handle_event_keyboard(ev, w)) {
				libinput_event_destroy(ev);
				gtk_main_quit();
				return FALSE;
			}
			break;
		}

		libinput_event_destroy(ev);
		libinput_dispatch(li);
	}
	gtk_widget_queue_draw(w->area);

	return TRUE;
}

static void
sockets_init(struct libinput *li)
{
	GIOChannel *c = g_io_channel_unix_new(libinput_get_fd(li));

	g_io_channel_set_encoding(c, NULL, NULL);
	g_io_add_watch(c, G_IO_IN, handle_event_libinput, li);
}

static int
parse_opts(int argc, char *argv[])
{
	while (1) {
		static struct option long_options[] = {
			{ "help", no_argument, 0, 'h' },
		};

		int option_index = 0;
		int c;

		c = getopt_long(argc, argv, "h", long_options,
				&option_index);
		if (c == -1)
			break;

		switch(c) {
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 1;
		}
	}

	return 0;
}

static int
open_restricted(const char *path, int flags, void *user_data)
{
	int fd = open(path, flags);
	return fd < 0 ? -errno : fd;
}

static void
close_restricted(int fd, void *user_data)
{
	close(fd);
}

static const struct libinput_interface interface = {
	.open_restricted = open_restricted,
	.close_restricted = close_restricted,
};

int
main(int argc, char *argv[])
{
	struct window w;
	struct libinput *li;
	struct udev *udev;

	gtk_init(&argc, &argv);

	if (parse_opts(argc, argv) != 0)
		return 1;

	udev = udev_new();
	if (!udev)
		error("Failed to initialize udev\n");

	li = libinput_udev_create_context(&interface, &w, udev);
	if (!li || libinput_udev_assign_seat(li, "seat0") != 0)
		error("Failed to initialize context from udev\n");

	window_init(&w);
	sockets_init(li);

	gtk_main();

	window_cleanup(&w);
	libinput_unref(li);
	udev_unref(udev);

	return 0;
}
