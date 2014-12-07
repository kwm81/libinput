#include "udev-stubs.h"

struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type,
                                                dev_t devnum) {
  fprintf(stderr, "udev_device_new_from_devnum\n");
  struct stat st;
  if (stat("/dev/input/event0", &st) != 0)
    return NULL;
  if (st.st_rdev == devnum) {
    fprintf(stderr, "  event0\n");
    return udev_device_new_from_syspath(udev, "/dev/input/event0");
  }
  if (stat("/dev/input/event1", &st) != 0)
    return NULL;
  if (st.st_rdev == devnum) {
    fprintf(stderr, "  event1\n");
    return udev_device_new_from_syspath(udev, "/dev/input/event1");
  }
  return NULL;
}
