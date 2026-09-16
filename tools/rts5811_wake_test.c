/*
 * Standalone test: replicate the proprietary driver's SensorReset()
 * precondition sequence (traced via static disassembly of
 * ~/focaltech-ft9366-arch-shim/libfprint-2.so.2.0.0) against the real
 * FT9366/RTS5811 hardware (2808:a658), then send the same CMD_INIT
 * (0xa5) the mainline libfprint focaltech_moc driver sends, to see
 * whether the precondition unblocks a response.
 *
 * Not linked against the proprietary binary in any way -- this is a
 * clean-room reimplementation of the byte sequence derived from
 * reading the disassembly.
 */
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define VID 0x2808
#define PID 0xa658
#define EP_OUT 0x01
#define EP_IN  0x82
#define TIMEOUT_MS 2000

static void hexdump(const char *label, const unsigned char *buf, int len)
{
    printf("%s (%d bytes):", label, len);
    for (int i = 0; i < len; i++) printf(" %02x", buf[i]);
    printf("\n");
}

static int bulk_write(libusb_device_handle *h, const unsigned char *buf, int len)
{
    int transferred = 0;
    int r = libusb_bulk_transfer(h, EP_OUT, (unsigned char *)buf, len, &transferred, TIMEOUT_MS);
    printf("  bulk_write -> ret=%d (%s) transferred=%d\n", r, libusb_error_name(r), transferred);
    if (r == 0) hexdump("  sent", buf, len);
    return r;
}

static int bulk_read(libusb_device_handle *h, unsigned char *buf, int len)
{
    int transferred = 0;
    int r = libusb_bulk_transfer(h, EP_IN, buf, len, &transferred, TIMEOUT_MS);
    printf("  bulk_read  -> ret=%d (%s) transferred=%d\n", r, libusb_error_name(r), transferred);
    if (r == 0 && transferred > 0) hexdump("  recv", buf, transferred);
    return r;
}

int main(void)
{
    libusb_context *ctx = NULL;
    libusb_device_handle *h = NULL;
    int r;

    r = libusb_init(&ctx);
    if (r != 0) { fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(r)); return 1; }

    h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) { fprintf(stderr, "device not found / cannot open\n"); return 1; }

    libusb_set_auto_detach_kernel_driver(h, 1);

    r = libusb_claim_interface(h, 0);
    if (r != 0) { fprintf(stderr, "claim_interface failed: %s\n", libusb_error_name(r)); return 1; }

    printf("== device opened and interface claimed ==\n\n");

    /* Real decode logic from ft_tell_mcu_capture_start, offsets into the
     * 4-byte response buffer: if resp[2]==2 -> return 1 (RETRY);
     * else if resp[3]==4 -> return 0 (PROCEED); else -> return -1 (PROCEED). */
    printf("== step 1: MCU capture-start ping (4c 5a 01 00) ==\n");
    unsigned char ping[4] = { 0x4c, 0x5a, 0x01, 0x00 };
    unsigned char resp[4];
    int attempt;
    int proceed = 0;
    for (attempt = 1; attempt <= 10; attempt++) {
        printf("-- attempt %d --\n", attempt);
        memset(resp, 0, sizeof(resp));
        int wr = bulk_write(h, ping, sizeof(ping));
        int rr = bulk_read(h, resp, sizeof(resp));
        if (wr != 0 || rr != 0) { usleep(50 * 1000); continue; }
        int decoded = (resp[2] == 0x02) ? 1 : (resp[3] == 0x04 ? 0 : -1);
        printf("  decoded = %d\n", decoded);
        if (decoded != 1) { proceed = 1; break; }
        usleep(50 * 1000); /* real retry delay */
    }

    if (!proceed) {
        printf("\n== step 1 kept returning RETRY after %d attempts. Proceeding anyway for diagnostic purposes. ==\n\n", attempt - 1);
    } else {
        printf("\n== step 1: decoded != 1, proceeding (attempt %d) ==\n\n", attempt);
    }

    printf("== step 2: sleep 10ms ==\n");
    usleep(10 * 1000);

    printf("== step 3: write 44 80 00 00 (no read pairing) ==\n");
    unsigned char wake[4] = { 0x44, 0x80, 0x00, 0x00 };
    bulk_write(h, wake, sizeof(wake));

    printf("== step 4: sleep 30ms then 100ms ==\n");
    usleep(30 * 1000);
    usleep(100 * 1000);

    printf("\n== step 5 [RULED OUT, kept only for regression reference]: generic FocalTech\n");
    printf("   CMD_INIT (02 00 01 a5 a4) -- traced disassembly confirms fw9366_init_chip()\n");
    printf("   never sends this envelope at all. Expect a timeout below; this is not a bug. ==\n");
    unsigned char cmd_init[5] = { 0x02, 0x00, 0x01, 0xa5, 0xa4 };
    bulk_write(h, cmd_init, sizeof(cmd_init));
    unsigned char final_resp[64];
    int fr = bulk_read(h, final_resp, sizeof(final_resp));
    if (fr == 0) {
        printf("\n*** UNEXPECTED: got a response to the ruled-out CMD_INIT. Re-open the investigation. ***\n");
    } else {
        printf("\n*** Timed out as expected -- confirms prior finding, not a new result. ***\n");
    }

    /* --- fw9366_cfg_init() reimplementation ---
     * Traced from static disassembly (address 0x155566 in the proprietary
     * .so). This function sends NOTHING to the device -- it only computes
     * host-side config state later consumed by fw9366_poa_send_para. There
     * is no hardware transfer here, so this section is local-only and
     * cannot produce a device response. smic_flag is not yet known (it's
     * set by fw9366_get_SMIC_IC_flag, not yet traced), so both branches are
     * printed for reference. */
    printf("\n== fw9366_cfg_init() local state (NOT a hardware test -- this function sends nothing) ==\n");
    unsigned char fw9366_cfg[0x12];
    memset(fw9366_cfg, 0, sizeof(fw9366_cfg));
    fw9366_cfg[0x0] = 0x78;
    fw9366_cfg[0x1] = 0x01;
    fw9366_cfg[0x2] = 0x01;
    fw9366_cfg[0x3] = 0x3c;
    fw9366_cfg[0x4] = 0xc8;
    /* Fw9366_cfg[0x2] == 0x01 (just set above) so this branch always taken */
    fw9366_cfg[0x5] = 0x04;
    fw9366_cfg[0x6] = 0x04;
    fw9366_cfg[0x7] = 0x32;
    fw9366_cfg[0x8] = 0x2d;
    fw9366_cfg[0x9] = 0x01;
    fw9366_cfg[0xa] = 0x02;
    fw9366_cfg[0xe] = 0x02;
    fw9366_cfg[0xf] = 0x32;
    fw9366_cfg[0x10] = 0x05;
    fw9366_cfg[0x11] = 0x08;

    unsigned char fw9366_cfg_smic_aa[0x12];
    memcpy(fw9366_cfg_smic_aa, fw9366_cfg, sizeof(fw9366_cfg));
    fw9366_cfg_smic_aa[0xc] = 0x96; fw9366_cfg_smic_aa[0xd] = 0x00;

    unsigned char fw9366_cfg_smic_other[0x12];
    memcpy(fw9366_cfg_smic_other, fw9366_cfg, sizeof(fw9366_cfg));
    fw9366_cfg_smic_other[0xc] = 0xc8; fw9366_cfg_smic_other[0xd] = 0x00;

    hexdump("  Fw9366_cfg if smic_flag==0xaa   ", fw9366_cfg_smic_aa, sizeof(fw9366_cfg_smic_aa));
    hexdump("  Fw9366_cfg if smic_flag!=0xaa   ", fw9366_cfg_smic_other, sizeof(fw9366_cfg_smic_other));
    printf("  (smic_flag itself is not yet known -- depends on fw9366_get_SMIC_IC_flag,\n");
    printf("   not yet traced. No hardware bytes were sent for this step.)\n");

    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
