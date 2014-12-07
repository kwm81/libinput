#ifndef UDEV_STUBS_H_
#define UDEV_STUBS_H_

#include <sys/stat.h>
#include <sys/types.h>

#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <unistd.h>

struct udev_device {
  int refcount;
  char const *syspath;
  char const *sysname;
};
struct udev {
  int refcount;
};
struct udev_list_entry {
  char const *path;
  struct udev_list_entry *next;
};
struct udev_monitor {
  int refcount;
  int fake_fds[2];
};
struct udev_enumerate {
  int refcount;
  struct udev_list_entry *dev_list;
};
#define va_list __va_list
inline char const *udev_device_get_devnode(struct udev_device *udev_device) {
  fprintf(stderr, "udev_device_get_devnode\n");
  return udev_device->syspath;
}
inline char const *udev_device_get_property_value(struct udev_device *dummy __unused, char const *property) {
  fprintf(stderr, "stub: udev_device_get_property_value %s\n", property);
  return NULL;
}
inline struct udev *udev_device_get_udev(struct udev_device *dummy __unused) {
  fprintf(stderr, "stub: udev_device_get_udev\n");
  return NULL;
}

inline struct udev_device *udev_device_new_from_syspath(struct udev *udev,
                                                        const char *syspath) {
  fprintf(stderr, "stub: udev_list_entry_get_name\n");
  struct udev_device *u = calloc(1, sizeof(struct udev_device));
  if (u) {
    u->refcount = 1;
    u->syspath = syspath;
    u->sysname = syspath + 11;
    return u;
  }
  return NULL;
}

struct udev_device *udev_device_new_from_devnum(struct udev *udev,
                                                       char type,
                                                       dev_t devnum);

inline const char *udev_device_get_syspath(struct udev_device *udev_device) {
  fprintf(stderr, "udev_device_get_syspath\n");
  return udev_device->syspath;
}
inline const char *udev_device_get_sysname(struct udev_device *udev_device) {
  fprintf(stderr, "stub: udev_device_get_sysname\n");
  return udev_device->sysname;
}
inline struct udev_device *udev_device_ref(struct udev_device *udev_device) {
  fprintf(stderr, "udev_device_ref\n");
  ++udev_device->refcount;
  return udev_device;
}
inline void udev_device_unref(struct udev_device *udev_device) {
  fprintf(stderr, "udev_device_unref %p %d\n", udev_device,
          udev_device->refcount);

  --udev_device->refcount;
  if (udev_device->refcount == 0) {
    free(udev_device);
  }
}
inline struct udev *udev_ref(struct udev *udev) {
  fprintf(stderr, "udev_ref\n");
  ++udev->refcount;
  return udev;
}
inline void udev_unref(struct udev *udev) {
  fprintf(stderr, "udev_unref\n");
  --udev->refcount;
  if (udev->refcount == 0) {
    free(udev);
  }
}

inline struct udev *udev_new(void) {
  fprintf(stderr, "udev_new\n");
  struct udev *u = calloc(1, sizeof(struct udev));
  if (u) {
    u->refcount = 1;
    return u;
  }
  return NULL;
}

inline struct udev_enumerate *udev_enumerate_new(struct udev *udev) {
  fprintf(stderr, "udev_enumerate_new\n");
  struct udev_enumerate *u = calloc(1, sizeof(struct udev_enumerate));
  if (u) {
    u->refcount = 1;
    return u;
  }
  return NULL;
}
inline int udev_enumerate_add_match_subsystem(
    struct udev_enumerate *udev_enumerate, const char *subsystem) {
  fprintf(stderr, "stub: udev_enumerate_add_match_subsystem\n");
  return 0;
}
inline int udev_enumerate_scan_devices(struct udev_enumerate *udev_enumerate) {
  fprintf(stderr, "udev_enumerate_scan_devices\n");

  struct udev_list_entry *le1 = calloc(1, sizeof(struct udev_list_entry));
  if (!le1)
    return -1;
  le1->path = "/dev/input/event0";
  udev_enumerate->dev_list = le1;

  struct udev_list_entry *le2 = calloc(1, sizeof(struct udev_list_entry));
  if (!le2) {
    free(le1);
    udev_enumerate->dev_list = NULL;
    return -1;
  }
  le2->path = "/dev/input/event1";
  udev_enumerate->dev_list->next = le2;

  return 0;
}

inline struct udev_list_entry *udev_enumerate_get_list_entry(
    struct udev_enumerate *udev_enumerate) {
  return udev_enumerate->dev_list;
}

#define udev_list_entry_foreach(list_entry, first_entry)                      \
  for (list_entry = first_entry; list_entry; list_entry = list_entry->next)

inline const char *udev_list_entry_get_name(
    struct udev_list_entry *list_entry) {
  fprintf(stderr, "udev_list_entry_get_name\n");
  return list_entry->path;
}

static inline void free_dev_list(struct udev_list_entry **list) {
  if (!*list)
    return;
  if ((*list)->next)
    free_dev_list(&(*list)->next);
  free(*list);
  *list = NULL;
}

inline void udev_enumerate_unref(struct udev_enumerate *udev_enumerate) {
  fprintf(stderr, "udev_enumerate_unref\n");
  --udev_enumerate->refcount;
  if (udev_enumerate->refcount == 0) {
    free(udev_enumerate);
    free_dev_list(&udev_enumerate->dev_list);
  }
}
inline struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev,
                                                          const char *name) {
  fprintf(stderr, "udev_monitor_new_from_netlink %p\n", udev);

  struct udev_monitor *u = calloc(1, sizeof(struct udev_monitor));
  if (!u) {
    return NULL;
  }

  if (pipe2(u->fake_fds, O_CLOEXEC) == -1) {
    free(u);
    return NULL;
  }

  u->refcount = 1;

  return u;
}
inline int udev_monitor_filter_add_match_subsystem_devtype(
    struct udev_monitor *udev_monitor, const char *subsystem,
    const char *devtype) {
  fprintf(stderr, "stub: udev_monitor_filter_add_match_subsystem_devtype\n");
  return 0;
}
inline int udev_monitor_enable_receiving(struct udev_monitor *udev_monitor) {
  fprintf(stderr, "stub: udev_monitor_enable_receiving\n");
  return 0;
}
inline int udev_monitor_get_fd(struct udev_monitor *udev_monitor) {
  fprintf(stderr, "udev_monitor_get_fd\n");
  return udev_monitor->fake_fds[0];
}
inline struct udev_device *udev_monitor_receive_device(
    struct udev_monitor *udev_monitor) {
  fprintf(stderr, "stub: udev_monitor_receive_device\n");
  return NULL;
}
inline const char *udev_device_get_action(struct udev_device *udev_device) {
  fprintf(stderr, "stub: udev_device_get_action\n");
  return NULL;
}
inline void udev_monitor_unref(struct udev_monitor *udev_monitor) {
  fprintf(stderr, "stub: udev_monitor_unref\n");
}

#endif
