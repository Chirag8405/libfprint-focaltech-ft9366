#include "focaltech-usb.h"

#define FT_USB_DEFAULT_TIMEOUT_MS 1500U

static guint
ft_usb_timeout_or_default(guint timeout_ms)
{
  return timeout_ms == 0U ? FT_USB_DEFAULT_TIMEOUT_MS : timeout_ms;
}

gboolean
ft_usb_vendor_in(GUsbDevice *device,
                 guint8 request,
                 guint16 value,
                 guint16 index,
                 guint8 *data,
                 gsize length,
                 gsize *actual_length,
                 guint timeout_ms,
                 GCancellable *cancellable,
                 GError **error)
{
  gsize transferred = 0;

  g_return_val_if_fail(G_USB_IS_DEVICE(device), FALSE);

  if (!g_usb_device_control_transfer(device,
                                     G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                     G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                     G_USB_DEVICE_RECIPIENT_DEVICE,
                                     request,
                                     value,
                                     index,
                                     data,
                                     length,
                                     &transferred,
                                     ft_usb_timeout_or_default(timeout_ms),
                                     cancellable,
                                     error)) {
    return FALSE;
  }

  if (actual_length != NULL)
    *actual_length = transferred;

  return TRUE;
}

gboolean
ft_usb_vendor_out(GUsbDevice *device,
                  guint8 request,
                  guint16 value,
                  guint16 index,
                  guint8 *data,
                  gsize length,
                  gsize *actual_length,
                  guint timeout_ms,
                  GCancellable *cancellable,
                  GError **error)
{
  gsize transferred = 0;

  g_return_val_if_fail(G_USB_IS_DEVICE(device), FALSE);

  if (!g_usb_device_control_transfer(device,
                                     G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                     G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                     G_USB_DEVICE_RECIPIENT_DEVICE,
                                     request,
                                     value,
                                     index,
                                     data,
                                     length,
                                     &transferred,
                                     ft_usb_timeout_or_default(timeout_ms),
                                     cancellable,
                                     error)) {
    return FALSE;
  }

  if (actual_length != NULL)
    *actual_length = transferred;

  return TRUE;
}

gboolean
ft_usb_bulk_in(GUsbDevice *device,
               guint8 endpoint,
               guint8 *data,
               gsize length,
               gsize *actual_length,
               guint timeout_ms,
               GCancellable *cancellable,
               GError **error)
{
  gsize transferred = 0;

  g_return_val_if_fail(G_USB_IS_DEVICE(device), FALSE);

  if (!g_usb_device_bulk_transfer(device,
                                  endpoint,
                                  data,
                                  length,
                                  &transferred,
                                  ft_usb_timeout_or_default(timeout_ms),
                                  cancellable,
                                  error)) {
    return FALSE;
  }

  if (actual_length != NULL)
    *actual_length = transferred;

  return TRUE;
}

gboolean
ft_usb_bulk_out(GUsbDevice *device,
                guint8 endpoint,
                guint8 *data,
                gsize length,
                gsize *actual_length,
                guint timeout_ms,
                GCancellable *cancellable,
                GError **error)
{
  gsize transferred = 0;

  g_return_val_if_fail(G_USB_IS_DEVICE(device), FALSE);

  if (!g_usb_device_bulk_transfer(device,
                                  endpoint,
                                  data,
                                  length,
                                  &transferred,
                                  ft_usb_timeout_or_default(timeout_ms),
                                  cancellable,
                                  error)) {
    return FALSE;
  }

  if (actual_length != NULL)
    *actual_length = transferred;

  return TRUE;
}

gboolean
ft_usb_interrupt_in(GUsbDevice *device,
                    guint8 endpoint,
                    guint8 *data,
                    gsize length,
                    gsize *actual_length,
                    guint timeout_ms,
                    GCancellable *cancellable,
                    GError **error)
{
  gsize transferred = 0;

  g_return_val_if_fail(G_USB_IS_DEVICE(device), FALSE);

  if (!g_usb_device_interrupt_transfer(device,
                                       endpoint,
                                       data,
                                       length,
                                       &transferred,
                                       ft_usb_timeout_or_default(timeout_ms),
                                       cancellable,
                                       error)) {
    return FALSE;
  }

  if (actual_length != NULL)
    *actual_length = transferred;

  return TRUE;
}
