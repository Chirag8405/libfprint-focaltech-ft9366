#pragma once

#include <gio/gio.h>
#include <glib.h>
#include <gusb.h>

G_BEGIN_DECLS

gboolean ft_usb_vendor_in(GUsbDevice *device,
                          guint8 request,
                          guint16 value,
                          guint16 index,
                          guint8 *data,
                          gsize length,
                          gsize *actual_length,
                          guint timeout_ms,
                          GCancellable *cancellable,
                          GError **error);

gboolean ft_usb_vendor_out(GUsbDevice *device,
                           guint8 request,
                           guint16 value,
                           guint16 index,
                           guint8 *data,
                           gsize length,
                           gsize *actual_length,
                           guint timeout_ms,
                           GCancellable *cancellable,
                           GError **error);

gboolean ft_usb_bulk_in(GUsbDevice *device,
                        guint8 endpoint,
                        guint8 *data,
                        gsize length,
                        gsize *actual_length,
                        guint timeout_ms,
                        GCancellable *cancellable,
                        GError **error);

gboolean ft_usb_bulk_out(GUsbDevice *device,
                         guint8 endpoint,
                         guint8 *data,
                         gsize length,
                         gsize *actual_length,
                         guint timeout_ms,
                         GCancellable *cancellable,
                         GError **error);

gboolean ft_usb_interrupt_in(GUsbDevice *device,
                             guint8 endpoint,
                             guint8 *data,
                             gsize length,
                             gsize *actual_length,
                             guint timeout_ms,
                             GCancellable *cancellable,
                             GError **error);

G_END_DECLS
