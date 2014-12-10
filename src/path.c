/*
 * Copyright © 2013 Red Hat, Inc.
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
#include <fcntl.h>
#include <string.h>
#ifdef __linux__
#include <libudev.h>
#else
#include "udev-stubs.h"
#endif

#include "path.h"
#include "evdev.h"

static const char default_seat[] = "seat0";
static const char default_seat_name[] = "default";

int path_input_process_event(struct libinput_event);
static void path_seat_destroy(struct libinput_seat *seat);

static void
path_disable_device(struct libinput *libinput,
		    struct evdev_device *device)
{
	struct libinput_seat *seat = device->base.seat;
	struct evdev_device *dev, *next;

	list_for_each_safe(dev, next,
			   &seat->devices_list, base.link) {
		if (dev != device)
			continue;

		evdev_device_remove(device);
		break;
	}
}

static void
path_input_disable(struct libinput *libinput)
{
	struct path_input *input = (struct path_input*)libinput;
	struct path_seat *seat, *tmp;
	struct evdev_device *device, *next;

	list_for_each_safe(seat, tmp, &input->base.seat_list, base.link) {
		libinput_seat_ref(&seat->base);
		list_for_each_safe(device, next,
				   &seat->base.devices_list, base.link)
			path_disable_device(libinput, device);
		libinput_seat_unref(&seat->base);
	}
}

static void
path_seat_destroy(struct libinput_seat *seat)
{
	struct path_seat *pseat = (struct path_seat*)seat;
	free(pseat);
}

static struct path_seat*
path_seat_create(struct path_input *input,
		 const char *seat_name,
		 const char *seat_logical_name)
{
	struct path_seat *seat;

	seat = zalloc(sizeof(*seat));
	if (!seat)
		return NULL;

	libinput_seat_init(&seat->base, &input->base, seat_name,
			   seat_logical_name, path_seat_destroy);

	return seat;
}

static struct path_seat*
path_seat_get_named(struct path_input *input,
		    const char *seat_name_physical,
		    const char *seat_name_logical)
{
	struct path_seat *seat;

	list_for_each(seat, &input->base.seat_list, base.link) {
		if (strcmp(seat->base.physical_name, seat_name_physical) == 0 &&
		    strcmp(seat->base.logical_name, seat_name_logical) == 0)
			return seat;
	}

	return NULL;
}

static struct libinput_device *
path_device_enable(struct path_input *input,
		   struct udev_device *udev_device,
		   const char *seat_logical_name_override)
{
	struct path_seat *seat;
	struct evdev_device *device = NULL;
	char *seat_name = NULL, *seat_logical_name = NULL;
	const char *seat_prop;
	const char *devnode;

	devnode = udev_device_get_devnode(udev_device);

	seat_prop = udev_device_get_property_value(udev_device, "ID_SEAT");
	seat_name = strdup(seat_prop ? seat_prop : default_seat);

	if (seat_logical_name_override) {
		seat_logical_name = strdup(seat_logical_name_override);
	} else {
		seat_prop = udev_device_get_property_value(udev_device, "WL_SEAT");
		seat_logical_name = strdup(seat_prop ? seat_prop : default_seat_name);
	}

	if (!seat_logical_name) {
		log_error(&input->base,
			  "failed to create seat name for device '%s'.\n",
			  devnode);
		goto out;
	}

	seat = path_seat_get_named(input, seat_name, seat_logical_name);

	if (seat) {
		libinput_seat_ref(&seat->base);
	} else {
		seat = path_seat_create(input, seat_name, seat_logical_name);
		if (!seat) {
			log_info(&input->base,
				 "failed to create seat for device '%s'.\n",
				 devnode);
			goto out;
		}
	}

	device = evdev_device_create(&seat->base, udev_device);
	libinput_seat_unref(&seat->base);

	if (device == EVDEV_UNHANDLED_DEVICE) {
		device = NULL;
		log_info(&input->base,
			 "not using input device '%s'.\n",
			 devnode);
		goto out;
	} else if (device == NULL) {
		log_info(&input->base,
			 "failed to create input device '%s'.\n",
			 devnode);
		goto out;
	}

out:
	free(seat_name);
	free(seat_logical_name);

	return device ? &device->base : NULL;
}

static int
path_input_enable(struct libinput *libinput)
{
	struct path_input *input = (struct path_input*)libinput;
	struct path_device *dev;

	list_for_each(dev, &input->path_list, link) {
		if (path_device_enable(input, dev->udev_device, NULL) == NULL) {
			path_input_disable(libinput);
			return -1;
		}
	}

	return 0;
}

static void
path_input_destroy(struct libinput *input)
{
	struct path_input *path_input = (struct path_input*)input;
	struct path_device *dev, *tmp;

	udev_unref(path_input->udev);

	list_for_each_safe(dev, tmp, &path_input->path_list, link) {
		udev_device_unref(dev->udev_device);
		free(dev);
	}

}

static struct libinput_device *
path_create_device(struct libinput *libinput,
		   struct udev_device *udev_device,
		   const char *seat_name)
{
	struct path_input *input = (struct path_input*)libinput;
	struct path_device *dev;
	struct libinput_device *device;

	dev = zalloc(sizeof *dev);
	if (!dev)
		return NULL;

	dev->udev_device = udev_device_ref(udev_device);

	list_insert(&input->path_list, &dev->link);

	device = path_device_enable(input, udev_device, seat_name);

	if (!device) {
		udev_device_unref(dev->udev_device);
		list_remove(&dev->link);
		free(dev);
	}

	return device;
}

static int
path_device_change_seat(struct libinput_device *device,
			const char *seat_name)
{
	struct libinput *libinput = device->seat->libinput;
	struct evdev_device *evdev_device = (struct evdev_device *)device;
	struct udev_device *udev_device = NULL;
	int rc = -1;

	udev_device = evdev_device->udev_device;
	udev_device_ref(udev_device);
	libinput_path_remove_device(device);

	if (path_create_device(libinput, udev_device, seat_name) != NULL)
		rc = 0;
	udev_device_unref(udev_device);
	return rc;
}

static const struct libinput_interface_backend interface_backend = {
	.resume = path_input_enable,
	.suspend = path_input_disable,
	.destroy = path_input_destroy,
	.device_change_seat = path_device_change_seat,
};

LIBINPUT_EXPORT struct libinput *
libinput_path_create_context(const struct libinput_interface *interface,
			     void *user_data)
{
	struct path_input *input;
	struct udev *udev;

	if (!interface)
		return NULL;

	udev = udev_new();
	if (!udev)
		return NULL;

	input = zalloc(sizeof *input);
	if (!input ||
	    libinput_init(&input->base, interface,
			  &interface_backend, user_data) != 0) {
		udev_unref(udev);
		free(input);
		return NULL;
	}

	input->udev = udev;
	list_init(&input->path_list);

	return &input->base;
}

static inline struct udev_device *
udev_device_from_devnode(struct udev *udev, const char *devnode)
{
	struct stat st;

	if (stat(devnode, &st) < 0)
		return NULL;

	return udev_device_new_from_devnum(udev, 'c', st.st_rdev);
}

LIBINPUT_EXPORT struct libinput_device *
libinput_path_add_device(struct libinput *libinput,
			 const char *path)
{
	struct path_input *input = (struct path_input *)libinput;
	struct udev *udev = input->udev;
	struct udev_device *udev_device;
	struct libinput_device *device;

	if (libinput->interface_backend != &interface_backend) {
		log_bug_client(libinput, "Mismatching backends.\n");
		return NULL;
	}

	udev_device = udev_device_from_devnode(udev, path);
	if (!udev_device) {
		log_bug_client(libinput, "Invalid path %s\n", path);
		return NULL;
	}

	device = path_create_device(libinput, udev_device, NULL);
	udev_device_unref(udev_device);
	return device;
}

LIBINPUT_EXPORT void
libinput_path_remove_device(struct libinput_device *device)
{
	struct libinput *libinput = device->seat->libinput;
	struct path_input *input = (struct path_input*)libinput;
	struct libinput_seat *seat;
	struct evdev_device *evdev = (struct evdev_device*)device;
	struct path_device *dev;

	if (libinput->interface_backend != &interface_backend) {
		log_bug_client(libinput, "Mismatching backends.\n");
		return;
	}

	list_for_each(dev, &input->path_list, link) {
		if (dev->udev_device == evdev->udev_device) {
			list_remove(&dev->link);
			udev_device_unref(dev->udev_device);
			free(dev);
			break;
		}
	}

	seat = device->seat;
	libinput_seat_ref(seat);
	path_disable_device(libinput, evdev);
	libinput_seat_unref(seat);
}
