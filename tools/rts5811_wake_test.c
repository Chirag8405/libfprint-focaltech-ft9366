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

    printf("\n== step 5: send CMD_INIT (02 00 01 a5 a4), same as mainline focaltech_moc driver ==\n");
    unsigned char cmd_init[5] = { 0x02, 0x00, 0x01, 0xa5, 0xa4 };
    bulk_write(h, cmd_init, sizeof(cmd_init));
    unsigned char final_resp[64];
    int fr = bulk_read(h, final_resp, sizeof(final_resp));
    if (fr == 0) {
        printf("\n*** GOT A RESPONSE TO CMD_INIT! Precondition sequence worked. ***\n");
    } else {
        printf("\n*** Still no response to CMD_INIT after precondition sequence. ***\n");
    }

    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
