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

/* fw9366_sfr_write(reg, val), traced at 0x165e31: fire-and-forget, bulk OUT
 * only via ff_spi_write_buf_rts (single write, no read pairing). */
static int sfr_write(libusb_device_handle *h, unsigned char reg, unsigned char val)
{
    unsigned char buf[4] = { 0x09, 0xf6, reg, val };
    return bulk_write(h, buf, sizeof(buf));
}

/* fw9366_otp_read(addr), traced at 0x166b99: address setup + fixed trigger
 * sequence via sfr_write, then a final sfr_read(0xf3) for the result. */
static int otp_read(libusb_device_handle *h, unsigned char addr, unsigned char *out)
{
    printf(" otp_read(0x%02x):\n", addr);
    sfr_write(h, 0xf1, addr);
    sfr_write(h, 0xf4, 0xc0);
    sfr_write(h, 0xf4, 0xc1);
    sfr_write(h, 0xf4, 0xc0);
    sfr_write(h, 0xf4, 0xc0);
    unsigned char cmd[5] = { 0x08, 0xf7, 0xf3, 0x00, 0x00 };
    unsigned char resp[1] = { 0 };
    int wr = bulk_write(h, cmd, sizeof(cmd));
    int rr = bulk_read(h, resp, sizeof(resp));
    if (wr != 0 || rr != 0) return -1;
    *out = resp[0];
    return 0;
}

/* 16-bit SRAM address space, distinct from the 8-bit SFR space above.
 * Address encoding (identical in both sram_write and sram_read, traced at
 * 0x1660e3 / 0x1663d0): enc_hi = ((addr>>8)&0x7f)|0x80, enc_lo = addr&0xff.
 * The "00 01" mid-field is a length-derived constant that happens to be
 * fixed for this function's always-2-byte address encoding. */
static void sram_encode_addr(unsigned short addr, unsigned char *hi, unsigned char *lo)
{
    *hi = (unsigned char)(((addr >> 8) & 0x7f) | 0x80);
    *lo = (unsigned char)(addr & 0xff);
}

/* fw9366_sram_write(addr, value), traced at 0x1660e3: write-only, opcode
 * [0x05, 0xfa]. */
static int sram_write(libusb_device_handle *h, unsigned short addr, unsigned short value)
{
    unsigned char hi, lo;
    sram_encode_addr(addr, &hi, &lo);
    unsigned char buf[8] = { 0x05, 0xfa, hi, lo, 0x00, 0x01,
                              (unsigned char)((value >> 8) & 0xff), (unsigned char)(value & 0xff) };
    return bulk_write(h, buf, sizeof(buf));
}

/* fw9366_sram_read(addr), traced at 0x1663d0: opcode [0x04, 0xfb], write 6
 * bytes, read 6 bytes, result = (resp[0]<<8)|resp[1]. This is the SAME sram
 * address space and read primitive used by fw9366_chipid_get() (addr
 * 0x1a8b) per the very first session's notes -- the original "chipid reads
 * 0x0" question this whole project started on. */
static int sram_read(libusb_device_handle *h, unsigned short addr, unsigned short *out)
{
    unsigned char hi, lo;
    sram_encode_addr(addr, &hi, &lo);
    unsigned char cmd[6] = { 0x04, 0xfb, hi, lo, 0x00, 0x01 };
    unsigned char resp[6] = { 0 };
    printf(" sram_read(0x%04x):\n", addr);
    int wr = bulk_write(h, cmd, sizeof(cmd));
    int rr = bulk_read(h, resp, sizeof(resp));
    if (wr != 0 || rr != 0) return -1;
    *out = (unsigned short)((resp[0] << 8) | resp[1]);
    return 0;
}

/* fw9366_sfr_read(reg), traced at 0x165f86 (used earlier inline for the
 * smic_flag loop; factored out here as a reusable helper now that more
 * callers need it). */
static int sfr_read(libusb_device_handle *h, unsigned char reg, unsigned char *out)
{
    unsigned char cmd[5] = { 0x08, 0xf7, reg, 0x00, 0x00 };
    unsigned char resp[1] = { 0 };
    int wr = bulk_write(h, cmd, sizeof(cmd));
    int rr = bulk_read(h, resp, sizeof(resp));
    if (wr != 0 || rr != 0) return -1;
    *out = resp[0];
    return 0;
}

/* FW9366_WorkMode_Cmd table, extracted directly from .rodata at 0x1c88c0
 * (3 bytes per mode, modes 0-11). Mode 11 sends only 1 byte; all others
 * send all 3. */
static const unsigned char FW9366_WorkMode_Cmd[12][3] = {
    { 0xc0, 0x3f, 0x00 }, { 0xc1, 0x3e, 0x00 }, { 0xc2, 0x3d, 0x00 }, { 0xc4, 0x3b, 0x00 },
    { 0xc8, 0x37, 0x00 }, { 0xd8, 0x27, 0x00 }, { 0xd1, 0x2e, 0x00 }, { 0xd2, 0x2d, 0x00 },
    { 0xd4, 0x2b, 0x00 }, { 0x5a, 0xa5, 0x00 }, { 0xa5, 0x5a, 0x00 }, { 0x70, 0x00, 0x00 },
};

/* fw9366_wm_switch(mode), traced at 0x165d48: table lookup + write-only
 * send, 3 bytes for modes 0-10, 1 byte for mode 11. */
static int wm_switch(libusb_device_handle *h, int mode)
{
    if (mode < 0 || mode > 11) return -1;
    int len = (mode == 11) ? 1 : 3;
    return bulk_write(h, FW9366_WorkMode_Cmd[mode], len);
}

/* fw9366_wm_get(), traced at 0x154c3b: sfr_read(0x80). */
static int wm_get(libusb_device_handle *h, unsigned char *out)
{
    return sfr_read(h, 0x80, out);
}

/* fw9366_idle_enter(), traced at 0x154c5a: fully self-contained, built
 * entirely on wm_switch/wm_get -- no unresolved dependencies. */
static int idle_enter(libusb_device_handle *h)
{
    printf(" idle_enter(): wm_switch(9)\n");
    wm_switch(h, 9);
    unsigned char wm = 0;
    printf(" idle_enter(): wm_get()\n");
    wm_get(h, &wm);
    printf("  wm_get() = 0x%02x\n", wm);
    if (wm != 0x50) {
        printf(" idle_enter(): wm != 0x50, wm_switch(0)\n");
        wm_switch(h, 0);
    }
    printf(" idle_enter(): wm_switch(0xa)\n");
    wm_switch(h, 0xa);
    return 0;
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

    /* --- fw9366_get_SMIC_IC_flag() / fw9366_sfr_read() reimplementation ---
     * Traced from static disassembly (fw9366_get_SMIC_IC_flag at 0x1557cc,
     * fw9366_sfr_read at 0x165f86, transport ff_spi_sfr_write_then_read_buf
     * at 0x153a4d). This IS a real hardware transfer -- distinct command
     * framing from the wake ping, but same underlying bulk EP 0x01/0x82
     * transport (same User_TL_Transmit_N_Byte path, confirmed by reading
     * ff_spi_sfr_write_then_read_buf's disassembly).
     *
     * fw9366_sfr_read(reg): write 5 bytes [0x08, 0xf7, reg, 0x00, 0x00],
     * read 1 byte back. Real device logic retries fw9366_sfr_read(0x9b) up
     * to 10 times (no sleep between attempts in the traced code), decoding
     * (byte >> 2): ==0x13 -> smic_flag=0xaa; ==0x00 -> smic_flag=0x00;
     * else -> retry. */
    printf("\n== fw9366_get_SMIC_IC_flag(): fw9366_sfr_read(0x9b), up to 10 attempts ==\n");
    int smic_flag = -1; /* -1 = undetermined after all attempts */
    for (int i = 0; i < 10; i++) {
        unsigned char sfr_cmd[5] = { 0x08, 0xf7, 0x9b, 0x00, 0x00 };
        unsigned char sfr_resp[1] = { 0 };
        printf("-- sfr_read attempt %d --\n", i + 1);
        int wr = bulk_write(h, sfr_cmd, sizeof(sfr_cmd));
        int rr = bulk_read(h, sfr_resp, sizeof(sfr_resp));
        if (wr != 0 || rr != 0) { continue; }
        /* traced code: movzx eax, byte (zero-extend) then sar eax,2 -- since
         * the zero-extended value is always 0-255, sar==shr here; use
         * unsigned shift to match exactly. */
        int shifted = ((unsigned int)sfr_resp[0]) >> 2;
        printf("  raw=0x%02x  (raw>>2)=0x%x\n", sfr_resp[0], shifted & 0xff);
        if (shifted == 0x13) { smic_flag = 0xaa; printf("  -> smic_flag = 0xaa\n"); break; }
        if (shifted == 0x00) { smic_flag = 0x00; printf("  -> smic_flag = 0x00\n"); break; }
        printf("  -> neither match, retrying\n");
    }
    if (smic_flag == -1) {
        printf("\n== smic_flag undetermined after 10 attempts (no match and/or all transfers failed) ==\n");
    } else {
        printf("\n== RESOLVED: smic_flag = 0x%02x -> Fw9366_cfg[0xc..0xd] = %s ==\n",
               smic_flag, smic_flag == 0xaa ? "0x0096" : "0x00c8");
    }

    /* --- fw9366_sfr_write() / fw9366_otp_read() / fw9366_Get_OTP_Info() ---
     * Traced from static disassembly: fw9366_sfr_write at 0x165e31 (buffer
     * [0x09, 0xf6, reg, val], write-only via ff_spi_write_buf_rts -- single
     * User_TL_Transmit_N_Byte write call, no read pairing), fw9366_otp_read
     * at 0x166b99, fw9366_Get_OTP_Info at 0x155a40. Real call in the init
     * chain is fw9366_Get_OTP_Info(NULL, NULL) -- both outputs discarded,
     * but the underlying reads still happen and are tested here. */
    printf("\n== fw9366_sfr_write() / fw9366_otp_read() ==\n");

    unsigned char otp3 = 0, otp13 = 0;
    int r3 = otp_read(h, 0x03, &otp3);
    int r13 = otp_read(h, 0x13, &otp13);
    if (r3 == 0 && r13 == 0) {
        printf("\n== RESOLVED: otp_read(3)&0x1f = 0x%02x, otp_read(0x13)&0x0f = 0x%02x ==\n",
               otp3 & 0x1f, otp13 & 0x0f);
    } else {
        printf("\n== otp_read failed (transfer error) ==\n");
    }

    /* fw9366_init_flag() (0x154cc2): pure local REG9366/fw9366_context state,
     * zero USB traffic -- not tested against hardware for the same reason
     * cfg_init wasn't: there is nothing to send. */
    printf("\n== fw9366_init_flag(): local state only, no hardware transfer (not tested) ==\n");

    /* fw9366_intflag_clear(0xffff) -> sram_write(0x1a84, 0xffff), traced at
     * 0x1549b3. Fire-and-forget, no response to check. */
    printf("\n== fw9366_intflag_clear(0xffff) -> sram_write(0x1a84, 0xffff) ==\n");
    sram_write(h, 0x1a84, 0xffff);

    /* Opportunistic bonus test, not strictly part of the traced init chain
     * order but cheap and directly relevant: fw9366_chipid_get() (from the
     * very first session's notes) is sram_read(0x1a8b). This is the exact
     * question this whole project started on -- report the raw result
     * without rounding up an ambiguous value into "it worked". */
    printf("\n== BONUS: fw9366_chipid_get() equivalent -> sram_read(0x1a8b) ==\n");
    unsigned short chipid = 0;
    int cr = sram_read(h, 0x1a8b, &chipid);
    if (cr == 0) {
        printf("\n== chipid raw result: 0x%04x ==\n", chipid);
        if (chipid == 0x9366) {
            printf("== MATCHES expected FT9366 chip ID exactly. ==\n");
        } else if (chipid == 0x0000) {
            printf("== Still reads as 0x0000 -- same failure mode as the original question, not resolved by this sequence alone. ==\n");
        } else {
            printf("== Non-zero but does NOT match 0x9366 -- new data point, not yet interpreted. Do not assume success. ==\n");
        }
    } else {
        printf("\n== chipid sram_read transfer failed/timed out ==\n");
    }

    /* --- fw9366_fdt_mode_init() -- INVOCATION A1 ONLY, per the full
     * call-site/state map in research/PROTOCOL.md (built BEFORE this code,
     * not alongside it). Of the 4 call sites in the binary, only 2 are
     * reachable from the real init chain, and of those, only the FIRST
     * invocation (from inside fw9366_fdt_AutoSDacUpdate's internal call to
     * fdt_manual_start) does real work -- the other two reachable
     * invocations are confirmed no-ops (fdt_mode_init's own state guard on
     * fw9366_context[0xfc] short-circuits them to a single log line). So
     * this IS the complete, correct opening sequence for the one call path
     * that matters, not a partial/arbitrary subset.
     *
     * CORRECTED from the previous session's test: a sram_write(0x180c, ...)
     * gated by AUTO_DAC_PRO_FLAG was missed entirely. At the real invocation
     * point AUTO_DAC_PRO_FLAG=1 (set by AutoSDacUpdate moments before this
     * call) and FW9366_LAST_AUTO=0xaa (a real compiled-in .data default, not
     * zero) -- the code superficially reads as if AUTO_DAC_PRO_FLAG==0 is
     * the default/normal path, but it is NOT at this invocation. Full
     * resolved sequence:
     *   fw9366_idle_enter()
     *   if (REG9366[0x77]==0): img_mode_init(0)   -- WILL run here in the
     *       real sequence (REG9366[0x77]=0 confirmed); img_mode_init itself
     *       (3074 bytes) is the next untraced piece -- deliberately NOT
     *       called here yet, same honest-gap policy as before.
     *   sram_write(0x1801, 0xfc9b)
     *   if (FW9366_LAST_AUTO(0xaa) != AUTO_DAC_PRO_FLAG(1)):     [true]
     *     if (AUTO_DAC_PRO_FLAG(1) == 0): ...                    [false, so:]
     *     else: sram_write(0x180c, sram_bits_set(0,hi=0xa,lo=0,new=0))
     *         = sram_write(0x180c, 0x0000)
     *     FW9366_LAST_AUTO = AUTO_DAC_PRO_FLAG   (host-side only, no wire effect)
     *   sram_write(0x1881, 0x0f0c)
     *   fw9366_context[0xfc] = 0xa1   (host-side only, no wire effect;
     *       this is what makes invocations A2/B into no-ops)
     *
     * Still not testing past this point: the rest of fdt_mode_init's body
     * (~60%) is unmapped, and img_mode_init (3074 bytes) is still untraced --
     * both real next steps, not being skipped silently. */
    printf("\n== fw9366_fdt_mode_init() INVOCATION A1 -- corrected, complete for this call path ==\n");
    printf("-- idle_enter() --\n");
    idle_enter(h);
    printf("-- [NOT CALLED: fw9366_img_mode_init(0) -- untraced, 3074 bytes; REG9366[0x77]==0 so real driver WOULD call this here] --\n");
    printf("-- sram_write(0x1801, 0xfc9b) --\n");
    sram_write(h, 0x1801, 0xfc9b);
    printf("-- sram_write(0x180c, 0x0000) [CORRECTED: AUTO_DAC_PRO_FLAG=1 branch, not the ==0 branch] --\n");
    sram_write(h, 0x180c, 0x0000);
    printf("-- sram_write(0x1881, 0x0f0c) --\n");
    sram_write(h, 0x1881, 0x0f0c);
    printf("\n== NOTE: this is invocation A1's confirmed sequence up to the point fdt_mode_init sets context[0xfc]=0xa1; img_mode_init(0) and the remaining ~60%% of fdt_mode_init's body are still not integrated ==\n");

    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
