#define FP_COMPONENT "focaltech_ft9366"

#include "focaltech-ft9366.h"

#include <fprint.h>
#include <libfprint/fpi-device.h>

#include "focaltech-usb.h"

#include <dlfcn.h>
#include <gio/gio.h>

typedef struct _FpiDeviceFocaltechFt9366 FpiDeviceFocaltechFt9366;
typedef struct _FpiDeviceFocaltechFt9366Class FpiDeviceFocaltechFt9366Class;

struct _FpiDeviceFocaltechFt9366 {
  FpDevice parent_instance;
  Ft9366Device transport;
  Ft9366ProtocolParams proto;
  gboolean transport_initialized;
};

struct _FpiDeviceFocaltechFt9366Class {
  FpDeviceClass parent_class;
};

static void fpi_device_focaltech_ft9366_init(FpiDeviceFocaltechFt9366 *self);
static void fpi_device_focaltech_ft9366_class_init(FpiDeviceFocaltechFt9366Class *klass);

#ifndef FPI_DEFINE_DRIVER
#define FPI_DEFINE_DRIVER(TypeName, type_name, parent_type) \
  G_DEFINE_TYPE(TypeName, type_name, parent_type)
#endif

FPI_DEFINE_DRIVER(FpiDeviceFocaltechFt9366,
                  fpi_device_focaltech_ft9366,
                  FP_TYPE_DEVICE)

const FpIdEntry focaltech_ft9366_id_table[] = {
  { .vid = FT9366_USB_VID, .pid = FT9366_USB_PID },
  { .vid = 0, .pid = 0, .driver_data = 0 }
};

typedef GUsbDevice *(*FtGetUsbDeviceFunc)(FpDevice *device);
typedef void (*FtOpenCompleteFunc)(FpDevice *device, GError *error);
typedef void (*FtCloseCompleteFunc)(FpDevice *device, GError *error);
typedef void (*FtEnrollCompleteFunc)(FpDevice *device, FpPrint *print, GError *error);
typedef void (*FtVerifyCompleteFunc)(FpDevice *device, GError *error);
typedef GError *(*FtDeviceErrorNewFunc)(FpDeviceError error);

static gpointer
ft9366_lookup_symbol(const gchar *name)
{
  return dlsym(RTLD_DEFAULT, name);
}

static GUsbDevice *
ft9366_get_usb_device(FpDevice *device)
{
  static FtGetUsbDeviceFunc get_usb_device = NULL;
  static gsize initialized = 0;
  GUsbDevice *usb = NULL;

  if (g_once_init_enter(&initialized)) {
    get_usb_device = (FtGetUsbDeviceFunc) ft9366_lookup_symbol("fpi_device_get_usb_device");
    g_once_init_leave(&initialized, 1);
  }

  if (get_usb_device != NULL)
    return get_usb_device(device);

  g_object_get(device, "fpi-usb-device", &usb, NULL);
  return usb;
}

static void
ft9366_open_complete(FpDevice *device, GError *error)
{
  static FtOpenCompleteFunc complete = NULL;
  static gsize initialized = 0;

  if (g_once_init_enter(&initialized)) {
    complete = (FtOpenCompleteFunc) ft9366_lookup_symbol("fpi_device_open_complete");
    g_once_init_leave(&initialized, 1);
  }

  if (complete != NULL)
    complete(device, error);
  else
    g_clear_error(&error);
}

static void
ft9366_close_complete(FpDevice *device, GError *error)
{
  static FtCloseCompleteFunc complete = NULL;
  static gsize initialized = 0;

  if (g_once_init_enter(&initialized)) {
    complete = (FtCloseCompleteFunc) ft9366_lookup_symbol("fpi_device_close_complete");
    g_once_init_leave(&initialized, 1);
  }

  if (complete != NULL)
    complete(device, error);
  else
    g_clear_error(&error);
}

static void
ft9366_enroll_complete_not_supported(FpDevice *device)
{
  static FtEnrollCompleteFunc complete = NULL;
  static FtDeviceErrorNewFunc device_error_new = NULL;
  static gsize initialized = 0;
  g_autoptr(GError) error = NULL;

  if (g_once_init_enter(&initialized)) {
    complete = (FtEnrollCompleteFunc) ft9366_lookup_symbol("fpi_device_enroll_complete");
    device_error_new = (FtDeviceErrorNewFunc) ft9366_lookup_symbol("fpi_device_error_new");
    g_once_init_leave(&initialized, 1);
  }

  if (complete == NULL || device_error_new == NULL)
    return;

  error = device_error_new(FP_DEVICE_ERROR_NOT_SUPPORTED);
  complete(device, NULL, g_steal_pointer(&error));
}

static void
ft9366_verify_complete_not_supported(FpDevice *device)
{
  static FtVerifyCompleteFunc complete = NULL;
  static FtDeviceErrorNewFunc device_error_new = NULL;
  static gsize initialized = 0;
  g_autoptr(GError) error = NULL;

  if (g_once_init_enter(&initialized)) {
    complete = (FtVerifyCompleteFunc) ft9366_lookup_symbol("fpi_device_verify_complete");
    device_error_new = (FtDeviceErrorNewFunc) ft9366_lookup_symbol("fpi_device_error_new");
    g_once_init_leave(&initialized, 1);
  }

  if (complete == NULL || device_error_new == NULL)
    return;

  error = device_error_new(FP_DEVICE_ERROR_NOT_SUPPORTED);
  complete(device, g_steal_pointer(&error));
}

static gboolean
focaltech_ensure_transport(FpiDeviceFocaltechFt9366 *self,
                           FpDevice *device,
                           GError **error)
{
  g_autoptr(GUsbDevice) usb = NULL;

  if (self->transport_initialized)
    return TRUE;

  usb = ft9366_get_usb_device(device);
  if (!G_USB_IS_DEVICE(usb)) {
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "libfprint did not provide a USB device handle");
    return FALSE;
  }

  ft9366_device_init(&self->transport, usb, &self->proto);
  self->transport_initialized = TRUE;
  return TRUE;
}

static void
focaltech_open(FpDevice *device)
{
  FpiDeviceFocaltechFt9366 *self = (FpiDeviceFocaltechFt9366 *) device;
  g_autoptr(GError) error = NULL;

  if (!focaltech_ensure_transport(self, device, &error) ||
      !ft9366_open(&self->transport, &error)) {
    g_warning("FT9366 open failed: %s", error != NULL ? error->message : "unknown error");
    ft9366_open_complete(device, g_steal_pointer(&error));
    return;
  }

  ft9366_open_complete(device, NULL);
}

static void
focaltech_close(FpDevice *device)
{
  FpiDeviceFocaltechFt9366 *self = (FpiDeviceFocaltechFt9366 *) device;
  g_autoptr(GError) error = NULL;

  if (!self->transport_initialized) {
    ft9366_close_complete(device, NULL);
    return;
  }

  if (!ft9366_close(&self->transport, &error)) {
    g_warning("FT9366 close failed: %s", error != NULL ? error->message : "unknown error");
    ft9366_close_complete(device, g_steal_pointer(&error));
    return;
  }

  ft9366_close_complete(device, NULL);
}

static void
focaltech_enroll(FpDevice *device)
{
  g_warning("FT9366 enroll callback invoked but enroll flow is not implemented yet");
  ft9366_enroll_complete_not_supported(device);
}

static void
focaltech_verify(FpDevice *device)
{
  g_warning("FT9366 verify callback invoked but verify flow is not implemented yet");
  ft9366_verify_complete_not_supported(device);
}

static void
fpi_device_focaltech_ft9366_finalize(GObject *object)
{
  FpiDeviceFocaltechFt9366 *self = (FpiDeviceFocaltechFt9366 *) object;

  if (self->transport_initialized) {
    ft9366_close(&self->transport, NULL);
    g_clear_object(&self->transport.usb);
    self->transport_initialized = FALSE;
  }

  G_OBJECT_CLASS(fpi_device_focaltech_ft9366_parent_class)->finalize(object);
}

static void
fpi_device_focaltech_ft9366_init(FpiDeviceFocaltechFt9366 *self)
{
  ft9366_protocol_params_set_defaults(&self->proto);
  self->transport_initialized = FALSE;
}

static void
fpi_device_focaltech_ft9366_class_init(FpiDeviceFocaltechFt9366Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->finalize = fpi_device_focaltech_ft9366_finalize;
  FpDeviceClass *dev_class = FP_DEVICE_CLASS(klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "FocalTech FT9366";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = focaltech_ft9366_id_table;
  dev_class->nr_enroll_stages = 8;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->features = FP_DEVICE_FEATURE_VERIFY;
  dev_class->open = focaltech_open;
  dev_class->close = focaltech_close;
  dev_class->enroll = focaltech_enroll;
  dev_class->verify = focaltech_verify;
}

/* Explicit declaration for TOD plugin scanners that resolve this symbol. */
GType
fpi_device_focaltech_ft9366_get_type(void);

static gboolean
ft9366_has_required_protocol(const Ft9366ProtocolParams *proto,
                             GError **error)
{
  g_return_val_if_fail(proto != NULL, FALSE);

  if (proto->request_read_chipid == FT9366_PROTO_CMD_UNKNOWN ||
      proto->request_set_fdt_mode == FT9366_PROTO_CMD_UNKNOWN ||
      proto->request_query_event_status == FT9366_PROTO_CMD_UNKNOWN) {
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        "Protocol commands unresolved. Populate request IDs from usbmon/Ghidra findings first.");
    return FALSE;
  }

  return TRUE;
}

void
ft9366_protocol_params_set_defaults(Ft9366ProtocolParams *params)
{
  g_return_if_fail(params != NULL);

  memset(params, 0, sizeof(*params));

  /*
   * These are transport defaults only. Command request IDs remain unknown
   * until a validated capture maps the control tuples.
   */
  params->request_read_chipid = FT9366_PROTO_CMD_UNKNOWN;
  params->request_set_fdt_mode = FT9366_PROTO_CMD_UNKNOWN;
  params->request_query_event_status = FT9366_PROTO_CMD_UNKNOWN;
  params->request_session_setup = FT9366_PROTO_CMD_UNKNOWN;
  params->endpoint_bulk_out = 0x02;
  params->endpoint_bulk_in = 0x81;
  params->endpoint_interrupt_in = 0x83;
  params->chipid_register = 0x1a8b;
  params->timeout_ms = 1500;
}

void
ft9366_device_init(Ft9366Device *device,
                   GUsbDevice *usb,
                   const Ft9366ProtocolParams *params)
{
  g_return_if_fail(device != NULL);
  g_return_if_fail(G_USB_IS_DEVICE(usb));

  memset(device, 0, sizeof(*device));
  device->usb = g_object_ref(usb);

  if (params != NULL)
    device->proto = *params;
  else
    ft9366_protocol_params_set_defaults(&device->proto);
}

gboolean
ft9366_open(Ft9366Device *device, GError **error)
{
  guint16 chipid = 0;

  g_return_val_if_fail(device != NULL, FALSE);
  g_return_val_if_fail(G_USB_IS_DEVICE(device->usb), FALSE);

  if (!ft9366_has_required_protocol(&device->proto, error))
    return FALSE;

  if (device->opened)
    return TRUE;

  if (!g_usb_device_open(device->usb, error))
    return FALSE;

  if (!ft9366_set_fdt_mode(device, FT9366_FDT_MODE_UP, error))
    goto fail;

  if (!ft9366_read_chipid(device, &chipid, error))
    goto fail;

  if (chipid != FT9366_EXPECTED_CHIP_ID) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "Unexpected chipid 0x%04x (expected 0x%04x)",
                chipid,
                FT9366_EXPECTED_CHIP_ID);
    goto fail;
  }

  device->opened = TRUE;
  return TRUE;

fail:
  g_usb_device_close(device->usb, NULL);
  return FALSE;
}

gboolean
ft9366_close(Ft9366Device *device, GError **error)
{
  g_return_val_if_fail(device != NULL, FALSE);

  ft_crypto_session_clear(&device->crypto);
  device->crypto_initialized = FALSE;

  if (device->usb != NULL && device->opened) {
    if (!g_usb_device_close(device->usb, error))
      return FALSE;
  }

  device->opened = FALSE;
  return TRUE;
}

gboolean
ft9366_read_chipid(Ft9366Device *device,
                   guint16 *chip_id,
                   GError **error)
{
  guint8 rsp[4] = {0};
  gsize actual = 0;
  guint16 le_chip = 0;
  guint16 be_chip = 0;

  g_return_val_if_fail(device != NULL, FALSE);

  if (!ft_usb_vendor_in(device->usb,
                        device->proto.request_read_chipid,
                        device->proto.chipid_register,
                        0,
                        rsp,
                        sizeof(rsp),
                        &actual,
                        device->proto.timeout_ms,
                        NULL,
                        error)) {
    return FALSE;
  }

  if (actual < 2) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                "Chipid response too short (%zu)",
                actual);
    return FALSE;
  }

  le_chip = (guint16) rsp[0] | ((guint16) rsp[1] << 8);
  be_chip = ((guint16) rsp[0] << 8) | (guint16) rsp[1];

  if (le_chip == FT9366_EXPECTED_CHIP_ID)
    device->chip_id = le_chip;
  else if (be_chip == FT9366_EXPECTED_CHIP_ID)
    device->chip_id = be_chip;
  else
    device->chip_id = le_chip;

  if (chip_id != NULL)
    *chip_id = device->chip_id;

  return TRUE;
}

gboolean
ft9366_set_fdt_mode(Ft9366Device *device,
                    Ft9366FdtMode mode,
                    GError **error)
{
  g_return_val_if_fail(device != NULL, FALSE);

  return ft_usb_vendor_out(device->usb,
                           device->proto.request_set_fdt_mode,
                           (guint16) mode,
                           0,
                           NULL,
                           0,
                           NULL,
                           device->proto.timeout_ms,
                           NULL,
                           error);
}

gboolean
ft9366_query_interrupt_status(Ft9366Device *device,
                              guint16 *status,
                              GError **error)
{
  guint8 rsp[2] = {0};
  gsize actual = 0;

  g_return_val_if_fail(device != NULL, FALSE);
  g_return_val_if_fail(status != NULL, FALSE);

  if (!ft_usb_vendor_in(device->usb,
                        device->proto.request_query_event_status,
                        0,
                        0,
                        rsp,
                        sizeof(rsp),
                        &actual,
                        device->proto.timeout_ms,
                        NULL,
                        error)) {
    return FALSE;
  }

  if (actual < 2) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                "Interrupt response too short (%zu)",
                actual);
    return FALSE;
  }

  *status = (guint16) rsp[0] | ((guint16) rsp[1] << 8);
  return TRUE;
}

gboolean
ft9366_crypto_start(Ft9366Device *device,
                    const guint8 *key,
                    gsize key_len,
                    const guint8 *iv,
                    gsize iv_len,
                    GError **error)
{
  g_return_val_if_fail(device != NULL, FALSE);

  if (!ft_crypto_session_init(&device->crypto, key, key_len, iv, iv_len, error))
    return FALSE;

  device->crypto_initialized = TRUE;
  return TRUE;
}

gboolean
ft9366_send_encrypted_bulk_command(Ft9366Device *device,
                                   const guint8 *plain_cmd,
                                   gsize plain_cmd_len,
                                   guint8 *plain_response,
                                   gsize plain_response_capacity,
                                   gsize *plain_response_len,
                                   GError **error)
{
  g_autofree guint8 *cipher_cmd = NULL;
  g_autofree guint8 *cipher_rsp = NULL;
  gsize cipher_cmd_written = 0;
  gsize usb_written = 0;
  gsize usb_read = 0;
  gsize plain_written = 0;

  g_return_val_if_fail(device != NULL, FALSE);
  g_return_val_if_fail(plain_cmd != NULL, FALSE);

  if (!device->crypto_initialized) {
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "Crypto is not initialized");
    return FALSE;
  }

  if (plain_cmd_len % FT_CRYPTO_AES_KEY_SIZE != 0U) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "Command length (%zu) must be AES block aligned",
                plain_cmd_len);
    return FALSE;
  }

  cipher_cmd = g_malloc0(plain_cmd_len);

  if (!ft_crypto_encrypt(&device->crypto,
                         plain_cmd,
                         plain_cmd_len,
                         cipher_cmd,
                         plain_cmd_len,
                         &cipher_cmd_written,
                         error)) {
    return FALSE;
  }

  if (!ft_usb_bulk_out(device->usb,
                       device->proto.endpoint_bulk_out,
                       cipher_cmd,
                       cipher_cmd_written,
                       &usb_written,
                       device->proto.timeout_ms,
                       NULL,
                       error)) {
    return FALSE;
  }

  if (plain_response == NULL || plain_response_capacity == 0)
    return TRUE;

  if (plain_response_capacity % FT_CRYPTO_AES_KEY_SIZE != 0U) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "Response buffer size (%zu) must be AES block aligned",
                plain_response_capacity);
    return FALSE;
  }

  cipher_rsp = g_malloc0(plain_response_capacity);

  if (!ft_usb_bulk_in(device->usb,
                      device->proto.endpoint_bulk_in,
                      cipher_rsp,
                      plain_response_capacity,
                      &usb_read,
                      device->proto.timeout_ms,
                      NULL,
                      error)) {
    return FALSE;
  }

  if (!ft_crypto_decrypt(&device->crypto,
                         cipher_rsp,
                         usb_read,
                         plain_response,
                         plain_response_capacity,
                         &plain_written,
                         error)) {
    return FALSE;
  }

  if (plain_response_len != NULL)
    *plain_response_len = plain_written;

  return TRUE;
}
