#pragma once

#include <gio/gio.h>
#include <glib.h>
#include <gusb.h>

#include "focaltech-crypto.h"

G_BEGIN_DECLS

#define FT9366_USB_VID 0x2808
#define FT9366_USB_PID 0xa658
#define FT9366_EXPECTED_CHIP_ID 0x9366
#define FT9366_PROTO_CMD_UNKNOWN 0xff

typedef enum {
  FT9366_FDT_MODE_UP = 0,
  FT9366_FDT_MODE_DOWN = 1,
  FT9366_FDT_MODE_WAIT_LEAVE = 2,
} Ft9366FdtMode;

typedef enum {
  FT9366_INT_IDLE = 0x0001,
  FT9366_INT_FINGER_DOWN = 0x0002,
  FT9366_INT_FINGER_UP = 0x0004,
  FT9366_INT_MANUAL = 0x0008,
  FT9366_INT_INVALID = 0x0010,
  FT9366_INT_DATA_READY = 0x0020,
  FT9366_INT_AFE_DOWN = 0x0040,
  FT9366_INT_HALF_FULL = 0x0080,
  FT9366_INT_FULL = 0x0100,
  FT9366_INT_RESET = 0x0200,
  FT9366_INT_ESD = 0x0400,
} Ft9366InterruptFlags;

typedef struct {
  guint8 request_read_chipid;
  guint8 request_set_fdt_mode;
  guint8 request_query_event_status;
  guint8 request_session_setup;
  guint8 endpoint_bulk_out;
  guint8 endpoint_bulk_in;
  guint8 endpoint_interrupt_in;
  guint16 chipid_register;
  guint timeout_ms;
} Ft9366ProtocolParams;

typedef struct {
  GUsbDevice *usb;
  Ft9366ProtocolParams proto;
  FtCryptoSession crypto;
  guint16 chip_id;
  gboolean opened;
  gboolean crypto_initialized;
} Ft9366Device;

void ft9366_protocol_params_set_defaults(Ft9366ProtocolParams *params);

void ft9366_device_init(Ft9366Device *device,
                        GUsbDevice *usb,
                        const Ft9366ProtocolParams *params);

gboolean ft9366_open(Ft9366Device *device, GError **error);

gboolean ft9366_close(Ft9366Device *device, GError **error);

gboolean ft9366_read_chipid(Ft9366Device *device,
                            guint16 *chip_id,
                            GError **error);

gboolean ft9366_set_fdt_mode(Ft9366Device *device,
                             Ft9366FdtMode mode,
                             GError **error);

gboolean ft9366_query_interrupt_status(Ft9366Device *device,
                                       guint16 *status,
                                       GError **error);

gboolean ft9366_crypto_start(Ft9366Device *device,
                             const guint8 *key,
                             gsize key_len,
                             const guint8 *iv,
                             gsize iv_len,
                             GError **error);

gboolean ft9366_send_encrypted_bulk_command(Ft9366Device *device,
                                            const guint8 *plain_cmd,
                                            gsize plain_cmd_len,
                                            guint8 *plain_response,
                                            gsize plain_response_capacity,
                                            gsize *plain_response_len,
                                            GError **error);

G_END_DECLS
