#pragma once

#if defined(__has_include_next)
#if __has_include_next(<libfprint/fpi-device.h>)
#include_next <libfprint/fpi-device.h>
#define FT9366_HAVE_SYSTEM_FPI_DEVICE_H 1
#endif
#endif

#ifndef FT9366_HAVE_SYSTEM_FPI_DEVICE_H

#include <fprint.h>
#include <gusb.h>

G_BEGIN_DECLS

typedef enum {
  FPI_DEVICE_UDEV_SUBTYPE_SPIDEV = 1 << 0,
  FPI_DEVICE_UDEV_SUBTYPE_HIDRAW = 1 << 1,
} FpiDeviceUdevSubtypeFlags;

typedef struct _FpIdEntry FpIdEntry;

struct _FpIdEntry
{
  union
  {
    struct
    {
      guint pid;
      guint vid;
    };
    const gchar *virtual_envvar;
    struct
    {
      FpiDeviceUdevSubtypeFlags udev_types;
      const gchar              *spi_acpi_id;
      struct
      {
        guint pid;
        guint vid;
      } hid_id;
    };
  };
  guint64 driver_data;
};

struct _FpDeviceClass
{
  GObjectClass parent_class;

  const gchar     *id;
  const gchar     *full_name;
  FpDeviceType     type;
  const FpIdEntry *id_table;
  FpDeviceFeature  features;

  gint       nr_enroll_stages;
  FpScanType scan_type;

  gint32 temp_hot_seconds;
  gint32 temp_cold_seconds;

  gint (*usb_discover) (GUsbDevice *usb_device);
  void (*probe)        (FpDevice *device);
  void (*open)         (FpDevice *device);
  void (*close)        (FpDevice *device);
  void (*enroll)       (FpDevice *device);
  void (*verify)       (FpDevice *device);
  void (*identify)     (FpDevice *device);
  void (*capture)      (FpDevice *device);
  void (*list)         (FpDevice *device);
  void (*delete)       (FpDevice *device);
  void (*clear_storage)(FpDevice *device);
  void (*cancel)       (FpDevice *device);
  void (*suspend)      (FpDevice *device);
  void (*resume)       (FpDevice *device);
};

void       fpi_device_class_auto_initialize_features(FpDeviceClass *device_class);
GUsbDevice *fpi_device_get_usb_device(FpDevice *device);
GError     *fpi_device_error_new(FpDeviceError error);

void fpi_device_open_complete(FpDevice *device,
                              GError   *error);
void fpi_device_close_complete(FpDevice *device,
                               GError   *error);
void fpi_device_enroll_complete(FpDevice *device,
                                FpPrint  *print,
                                GError   *error);
void fpi_device_verify_complete(FpDevice *device,
                                GError   *error);

G_END_DECLS

#endif
