#ifndef UDEV_STUBS_H_
#define UDEV_STUBS_H_

#include <sys/types.h>
#include <stdio.h>
struct udev_device {
  int dummy;
};
struct udev {
  int dummy;
};
struct udev_list_entry {
  int dummy;
};
#define va_list __va_list
inline char const *udev_device_get_devnode(struct udev_device *dummy
                                               __unused) {
  fprintf(stderr, "stub: udev_device_get_devnode\n");
  return NULL;
}
inline char const *udev_device_get_property_value(struct udev_device *dummy __unused, char const *property) {
  fprintf(stderr, "stub: udev_device_get_property_value %s\n", property);
  return NULL;
}
inline struct udev *udev_device_get_udev(struct udev_device *dummy __unused) {
  fprintf(stderr, "stub: udev_device_get_udev\n");
  return NULL;
}
inline struct udev_device *udev_device_new_from_devnum(struct udev *udev,
                                                       char type,
                                                       dev_t devnum) {
  fprintf(stderr, "stub: udev_device_new_from_devnum\n");
  return NULL;
}
inline const char *udev_device_get_syspath(struct udev_device *udev_device) {
  fprintf(stderr, "stub: udev_device_get_syspath\n");
  return NULL;
}
inline const char *udev_device_get_sysname(struct udev_device *udev_device) {
  fprintf(stderr, "stub: udev_device_get_sysname\n");
  return NULL;
}
inline struct udev_device *udev_device_ref(struct udev_device *udev_device) {
  fprintf(stderr, "stub: udev_device_ref\n");
  return NULL;
}
inline void udev_device_unref(struct udev_device *udev_device) {
  fprintf(stderr, "stub: udev_device_unref\n");
}
inline struct udev *udev_ref(struct udev *udev) {
  fprintf(stderr, "stub: udev_ref\n");
  return NULL;
}
inline void udev_unref(struct udev *udev) {
  fprintf(stderr, "stub: udev_unref\n");
}
inline struct udev *udev_new(void) {
  fprintf(stderr, "stub: udev_new\n");
  return NULL;
}
inline struct udev_enumerate *udev_enumerate_new(struct udev *udev) {
  fprintf(stderr, "stub: udev_enumerate_new\n");
  return NULL;
}
inline int udev_enumerate_add_match_subsystem(
    struct udev_enumerate *udev_enumerate, const char *subsystem) {
  fprintf(stderr, "stub: udev_enumerate_add_match_subsystem\n");
  return 0;
}
inline int udev_enumerate_scan_devices(struct udev_enumerate *udev_enumerate) {
  fprintf(stderr, "stub: udev_enumerate_scan_devices\n");
  return 0;
}

#define udev_list_entry_foreach(list_entry, first_entry) while (0)

inline const char *udev_list_entry_get_name(
    struct udev_list_entry *list_entry) {
  fprintf(stderr, "stub: udev_list_entry_get_name\n");
  return NULL;
}
inline struct udev_device *udev_device_new_from_syspath(struct udev *udev,
                                                        const char *syspath) {
  fprintf(stderr, "stub: udev_list_entry_get_name\n");
  return NULL;
}
inline void udev_enumerate_unref(struct udev_enumerate *udev_enumerate) {
  fprintf(stderr, "stub: udev_enumerate_unref\n");
}
inline struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev,
                                                          const char *name) {
  fprintf(stderr, "stub: udev_monitor_new_from_netlink\n");
  return NULL;
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
  fprintf(stderr, "stub: udev_monitor_get_fd\n");
  return -1;
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
