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
#include <stdlib.h>
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
    if (r == 0 && transferred > 0) {
        if (transferred <= 64) {
            hexdump("  recv", buf, transferred);
        } else {
            /* Large transfer (e.g. an image chunk) -- avoid flooding the
             * terminal with a full hexdump; show a short preview plus
             * basic stats useful for judging whether the data looks real
             * (varied) vs. flat/empty. */
            hexdump("  recv (first 32 bytes)", buf, 32);
            unsigned int sum = 0, nonzero = 0;
            unsigned char minv = 0xff, maxv = 0;
            for (int i = 0; i < transferred; i++) {
                unsigned char b = buf[i];
                sum += b;
                if (b != 0) nonzero++;
                if (b < minv) minv = b;
                if (b > maxv) maxv = b;
            }
            printf("  [%d bytes total] nonzero=%u/%d min=0x%02x max=0x%02x avg=%.1f\n",
                   transferred, nonzero, transferred, minv, maxv, (double)sum / transferred);
        }
    }
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

/* fw9366_sram_bits_set(value, hi, lo, new_bits), traced at 0x154d7d: pure
 * local bitfield helper, no I/O. width = hi-lo+1; clear those bits in
 * value, then OR in (new_bits << lo) masked to that width. */
static unsigned short sram_bits_set(unsigned short value, int hi, int lo, unsigned short new_bits)
{
    int width = hi - lo + 1;
    unsigned short mask = (unsigned short)(((1u << width) - 1u) << lo);
    unsigned short cleared = (unsigned short)(value & ~mask);
    return (unsigned short)(cleared | ((new_bits << lo) & mask));
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

/* General length-field encoding used by both sram_read_bulk_withecc and
 * fw9366_fifo_read: half = len/2, stored BIG-ENDIAN as two bytes
 * (high-byte-of-half first, low-byte-of-half second). Re-derived precisely
 * (not just pattern-matched) after noticing the original simplified
 * "cmd[5]=half as one byte" shortcut only happened to work for the small
 * lengths tested so far (half<256) -- it silently breaks for real image
 * chunk sizes (e.g. half=5120 for a 10240-byte chunk, which doesn't fit
 * in one byte). Verified against both previously-confirmed small cases
 * (len=2 -> 00 01, len=8 -> 00 04) before use. */
static void encode_len_field(unsigned short len, unsigned char *b_hi, unsigned char *b_lo)
{
    unsigned short half = (unsigned short)(len / 2);
    *b_hi = (unsigned char)((half >> 8) & 0xff);
    *b_lo = (unsigned char)(half & 0xff);
}

/* fw9366_sram_read_bulk_withecc(addr, out_buf, len_words), traced at
 * 0x1666cb: same address encoding as sram_read/write, dynamic length
 * field (general formula above). IMPORTANT: the actual bytes read back is
 * (len_words-2), not len_words -- confirmed from the disassembly's
 * internal length variable, not assumed. */
static int sram_read_bulk_withecc(libusb_device_handle *h, unsigned short addr,
                                    unsigned char *out_buf, unsigned short len_words)
{
    unsigned char hi, lo, len_hi, len_lo;
    sram_encode_addr(addr, &hi, &lo);
    unsigned short internal_len = (unsigned short)(len_words - 2);
    encode_len_field(internal_len, &len_hi, &len_lo);
    unsigned char cmd[6] = { 0x04, 0xfb, hi, lo, len_hi, len_lo };
    printf(" sram_read_bulk_withecc(0x%04x, len_words=%u -> actual %u bytes):\n", addr, len_words, internal_len);
    int wr = bulk_write(h, cmd, sizeof(cmd));
    int rr = bulk_read(h, out_buf, internal_len);
    if (wr != 0 || rr != 0) return -1;
    return 0;
}

/* fw9366_fifo_read(addr, out_buf, len), traced at 0x166891: opcode
 * [0x06, 0xf9], same address encoding, general length-field encoding
 * (see encode_len_field). UNLIKE sram_read_bulk_withecc, there is NO -2
 * adjustment here -- the actual bytes read back equals len exactly,
 * confirmed by the disassembly not applying any subtraction to the
 * length variable before it's used as both the field-encoding input and
 * the final USB read length. Uses a different low-level transport
 * (ff_spi_read_image_buf vs ff_spi_write_then_read_buf_rts) but it's
 * structurally identical (same User_TL_Transmit_N_Byte write/read pair,
 * same bulk EP 0x01/0x82) -- no special large-transfer handling needed;
 * libusb_bulk_transfer already handles multi-packet transfers. */
static int fifo_read(libusb_device_handle *h, unsigned short addr,
                       unsigned char *out_buf, unsigned int len)
{
    unsigned char hi, lo, len_hi, len_lo;
    sram_encode_addr(addr, &hi, &lo);
    encode_len_field((unsigned short)len, &len_hi, &len_lo);
    unsigned char cmd[6] = { 0x06, 0xf9, hi, lo, len_hi, len_lo };
    printf(" fifo_read(0x%04x, len=%u):\n", addr, len);
    int wr = bulk_write(h, cmd, sizeof(cmd));
    int rr = bulk_read(h, out_buf, (int)len);
    if (wr != 0 || rr != 0) return -1;
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

/* FW9366_INT_INDEX table, extracted from .rodata at 0x1c6000: bit-flag
 * table, entry[i] = 1<<i (only indices 0-10 needed so far). */
static const unsigned short FW9366_INT_INDEX[11] = {
    0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080, 0x0100, 0x0200, 0x0400,
};

/* fw9366_intflag_mask(src), traced at 0x1549e1: sram_read(0x1a83) | mask,
 * sram_write(0x1a83, result). mask = FW9366_INT_INDEX[src]. */
static int intflag_mask(libusb_device_handle *h, int src)
{
    unsigned short v = 0;
    sram_read(h, 0x1a83, &v);
    v = (unsigned short)(v | FW9366_INT_INDEX[src]);
    return sram_write(h, 0x1a83, v);
}

/* fw9366_int_gap_set(gap), traced at 0x154b50: gap clamped to <=0x68, then
 * sfr_write(0x8e, (gap*10000)>>12). */
static int int_gap_set(libusb_device_handle *h, unsigned char gap)
{
    if (gap > 0x68) gap = 0x68;
    int computed = ((int)gap * 10000) >> 12;
    return sfr_write(h, 0x8e, (unsigned char)computed);
}

/* fw9366_wdtcnt_int_en(en), traced at 0x154ba7: sfr_write(0x90, en?1:0). */
static int wdtcnt_int_en(libusb_device_handle *h, int en)
{
    return sfr_write(h, 0x90, en ? 1 : 0);
}

/* fw9366_wdtcnt_gap_set(val), traced at 0x154bdd. */
static int wdtcnt_gap_set(libusb_device_handle *h, unsigned short val)
{
    wdtcnt_int_en(h, 0);
    sfr_write(h, 0x91, (unsigned char)((val >> 8) & 0xff));
    sfr_write(h, 0x92, (unsigned char)(val & 0xff));
    wdtcnt_int_en(h, 1);
    return 0;
}

/* fw9366_fdt_block(), traced at 0x155c3d: pure local, returns 4 since
 * Fw9366_cfg[2]==1 (confirmed always true), no I/O. */
static int fdt_block(void) { return 4; }

/* fw9366_fdt_get_a_frame_data(out_buf), traced at 0x155c5e. Calls
 * fdt_block() (=4, local), branches on smic_flag (confirmed =0 for this
 * unit, live-measured earlier -- NOT 0xaa, so our path uses address 0xb8),
 * bulk-reads via sram_read_bulk_withecc, then byte-swaps each of the
 * `block` 16-bit words in place. out_buf must be at least 2*block bytes
 * (8 bytes for block=4). */
static int fdt_get_a_frame_data(libusb_device_handle *h, unsigned char *out_buf)
{
    int block = fdt_block();
    unsigned short len_words = (unsigned short)((block + 1) * 2);
    unsigned short addr = 0xb8; /* smic_flag=0 != 0xaa confirmed -> this branch */
    sram_read_bulk_withecc(h, addr, out_buf, len_words);
    for (int i = 0; i < block; i++) {
        unsigned char b0 = out_buf[i * 2];
        unsigned char b1 = out_buf[i * 2 + 1];
        out_buf[i * 2] = b1;
        out_buf[i * 2 + 1] = b0;
    }
    return 0;
}


/* fw9392_fdt_base_fail_check(data), traced at 0x155d26: pure local, no I/O.
 * The real function reads each entry as a native (little-endian x86)
 * uint16 from the buffer produced by fdt_get_a_frame_data -- which already
 * has the byte-swap applied. So here we must also do a plain little-endian
 * read of that same already-swapped buffer, not reinterpret the byte
 * order again. Valid range is 299 < val <= 0x2bc(700). Returns -1 (fail)
 * immediately on the first out-of-range value, else 0 after checking all 4. */
static int fdt_base_fail_check(const unsigned char *swapped_buf)
{
    for (int i = 0; i < 4; i++) {
        unsigned short val = (unsigned short)(swapped_buf[i * 2] | (swapped_buf[i * 2 + 1] << 8));
        if (val > 0x2bc) {
            printf("  fdt_base_fail_check: i=%d val=%u (0x%04x) > 0x2bc -- FAIL\n", i, val, val);
            return -1;
        }
        if (val <= 0x12b) {
            printf("  fdt_base_fail_check: i=%d val=%u (0x%04x) <= 0x12b -- FAIL\n", i, val, val);
            return -1;
        }
        printf("  fdt_base_fail_check: i=%d val=%u (0x%04x) -- OK\n", i, val, val);
    }
    return 0;
}

/* fw9366_calculate_crc(data, len), traced at 0x166c09: pure local, no I/O.
 * Standard CRC-16/CCITT-FALSE: poly=0x1021, init=0xffff, MSB-first, no
 * reflection, no final XOR. VERIFIED against the standard test vector
 * ("123456789" -> 0x29b1) before use here, not assumed correct from
 * pattern-matching the disassembly alone. */
static unsigned short calculate_crc(const unsigned char *data, unsigned int len)
{
    unsigned short crc = 0xffff;
    for (unsigned int i = 0; i < len; i++) {
        unsigned short cur = (unsigned short)(data[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            unsigned short xor_val = (unsigned short)(crc ^ cur);
            crc = (unsigned short)(crc << 1);
            cur = (unsigned short)(cur << 1);
            if (xor_val & 0x8000) crc ^= 0x1021;
        }
    }
    return crc;
}

/* fw9366_fdt_base_Min_Updata(data), traced at 0x15a832: pure local, no I/O
 * at all (no sram/sfr calls anywhere in its body). Operates entirely on
 * the frame_buf already captured by fdt_get_a_frame_data and produces two
 * CRC values. Since it has zero wire effect, there is nothing new to test
 * against hardware here -- verified via the CRC algorithm's standard test
 * vector instead (see calculate_crc above). block=fdt_block()=4 confirmed
 * always. The real function stores results into REG9366 at various
 * offsets (0x92+2i, 0xa6+2i, 0xba+2i, 0xca, 0xb6) -- this session's tool
 * doesn't maintain a full REG9366 mirror, so we compute the two CRCs
 * directly from the same source data (the adjusted frame values), since
 * both CRC'd regions are confirmed to be populated with an identical copy
 * of that same adjusted data. */
static void fdt_base_min_updata(unsigned char *swapped_buf, unsigned short *crc1_out, unsigned short *crc2_out)
{
    unsigned short adjusted[4];
    for (int i = 0; i < 4; i++) {
        unsigned short val = (unsigned short)(swapped_buf[i * 2] | (swapped_buf[i * 2 + 1] << 8));
        adjusted[i] = (val <= 0x1e) ? 0 : (unsigned short)(val - 0x1e);
        printf("  fdt_base_min_updata: i=%d raw=%u adjusted=%u\n", i, val, adjusted[i]);
    }
    /* Both CRC'd regions (REG9366+0xba and REG9366+0xa6) are populated with
     * the same adjusted data in the real function -- compute both CRCs
     * over that identical 8-byte buffer (native u16 layout, matching how
     * REG9366 would store it). */
    unsigned char crc_input[8];
    for (int i = 0; i < 4; i++) {
        crc_input[i * 2] = (unsigned char)(adjusted[i] & 0xff);
        crc_input[i * 2 + 1] = (unsigned char)((adjusted[i] >> 8) & 0xff);
    }
    *crc1_out = calculate_crc(crc_input, 8);
    *crc2_out = calculate_crc(crc_input, 8);
}

/* Img_Get_Avg_Middle(image_buf), traced at 0x15d574: pure local, no I/O.
 * Median-via-histogram over a sub-region (rows 1-78, cols 2-61, stride=64)
 * of the captured image, pixel values scaled by 1/(1<<Fw9366_cfg[0xa]).
 * Fw9366_cfg[0xa]=2 confirmed via cfg_init -> divisor=4.
 *
 * IMPORTANT independent confirmation: this function's stride=64 matches
 * this session's earlier VISUALLY-derived image width exactly -- the real
 * firmware's own statistics code uses the same row width, not a
 * coincidental reshape guess.
 *
 * BYTE ORDER CAUGHT AND CORRECTED: the literal disassembly shows a native
 * (x86 little-endian) `movzx eax, WORD PTR [addr]` read directly on the
 * raw captured buffer, with no visible byte-swap step anywhere in
 * image_read/fifo_read/img_data_get (unlike the small frame_data path,
 * which explicitly swaps). Taking that literally and reading the real
 * captured data as little-endian produces NONSENSE (median pegged at the
 * max bucket 1023, average ~31000 -- consistent with near-random data).
 * Reading the SAME real data as big-endian produces physically plausible
 * values (baseline median=665, touch median=489) consistent with the
 * already-confirmed systematic brightness decrease under touch, and
 * matches the byte order already used for the visually-confirmed ridge
 * image. VALIDATED against real captured data (research/captures/ raw files)
 * before trusting either interpretation -- the literal disassembly read
 * turned out to be misleading (there is very likely a byte-swap step
 * somewhere in the real chain not yet located), so this implementation
 * uses the empirically-correct big-endian interpretation. */
static int img_get_avg_middle(const unsigned char *image_be_pairs, int image_len_bytes)
{
    int divisor = 1 << 2; /* Fw9366_cfg[0xa]=2 confirmed */
    int width = 64;
    int histogram[1024] = {0};
    int total = 0;
    int npixels = image_len_bytes / 2;
    for (int row = 1; row <= 78; row++) {
        for (int col = 2; col <= 61; col++) {
            int idx = row * width + col;
            if (idx >= npixels) continue;
            unsigned short pixel = (unsigned short)((image_be_pairs[idx * 2] << 8) | image_be_pairs[idx * 2 + 1]);
            int bucket = pixel / divisor;
            if (bucket > 0x3ff) bucket = 0x3ff;
            histogram[bucket]++;
            total++;
        }
    }
    int sum = 0;
    for (int bucket = 0; bucket < 1024; bucket++) {
        sum += histogram[bucket];
        if (sum > total / 2) return bucket;
    }
    return 1023;
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

/* fw9366_Set_Scan_Rate_2M(), traced at 0x15b71c: three live
 * read-modify-write ops on real SRAM registers, no host-state
 * dependency at all -- each starts from a fresh sram_read of the actual
 * current hardware value, so this is safe to replicate exactly regardless
 * of any other state question. */
static int set_scan_rate_2m(libusb_device_handle *h)
{
    unsigned short v;
    printf(" set_scan_rate_2m(): 0x1806\n");
    sram_read(h, 0x1806, &v);
    v = sram_bits_set(v, 13, 7, 9);
    sram_write(h, 0x1806, v);

    printf(" set_scan_rate_2m(): 0x180a\n");
    sram_read(h, 0x180a, &v);
    v = sram_bits_set(v, 13, 7, 9);
    v = sram_bits_set(v, 6, 0, 3);
    sram_write(h, 0x180a, v);

    printf(" set_scan_rate_2m(): 0x180b\n");
    sram_read(h, 0x180b, &v);
    v = sram_bits_set(v, 13, 7, 4);
    v = sram_bits_set(v, 6, 0, 8);
    sram_write(h, 0x180b, v);
    return 0;
}

/* fw9366_img_mode_init(0), traced at 0x15b8a9 -- fully traced, 100%, and
 * tested clean against real hardware (see research/PROTOCOL.md). Factored
 * out as a real function since fw9366_img_scan_start also calls it (a
 * second invocation, with fw9366_context[0xfc] now at 0xa1 rather than
 * 0xa0 -- but the function's OWN internal gate checks against 0xa3, so a
 * second call at state 0xa1 still runs the full body again, identically). */
static void img_mode_init_0(libusb_device_handle *h)
{
    printf("-- img_mode_init(0) opening: idle_enter() again --\n");
    idle_enter(h);
    printf("-- img_mode_init(0): sram_write(0x1801, 0xfcb6) [REG9366[0x87]=0x36] --\n");
    sram_write(h, 0x1801, 0xfcb6);
    printf("-- img_mode_init(0): sram_write(0x1800, 0x4ffe) [fixed constant, param==0 branch] --\n");
    sram_write(h, 0x1800, 0x4ffe);
    printf("-- img_mode_init(0): sram_write(0x1804, 0x27ca) --\n");
    sram_write(h, 0x1804, 0x27ca);
    printf("-- img_mode_init(0): set_scan_rate_2m() --\n");
    set_scan_rate_2m(h);
    printf("-- img_mode_init(0): sram_write(0x1807, 0x18e1) --\n");
    sram_write(h, 0x1807, 0x18e1);
    printf("-- img_mode_init(0): sram_write(0x1887, 2) --\n");
    sram_write(h, 0x1887, 2);
    printf("-- img_mode_init(0): 0x1805 live read-modify-write (clear low byte) --\n");
    {
        unsigned short v = 0;
        sram_read(h, 0x1805, &v);
        v = sram_bits_set(v, 4, 0, 0);
        v = sram_bits_set(v, 7, 5, 0);
        sram_write(h, 0x1805, v);
    }
    printf("-- img_mode_init(0): 0x1811 live read-modify-write --\n");
    {
        unsigned short v = 0;
        sram_read(h, 0x1811, &v);
        v = sram_bits_set(v, 9, 0, 0x1fe);
        sram_write(h, 0x1811, v);
    }
    printf("-- img_mode_init(0): intflag_mask(5) --\n");
    intflag_mask(h, 5);
    printf("-- img_mode_init(0): intflag_mask(6) --\n");
    intflag_mask(h, 6);
    printf("-- img_mode_init(0): int_gap_set(0x64) --\n");
    int_gap_set(h, 0x64);
    printf("-- img_mode_init(0): wdtcnt_gap_set(0x7d0) --\n");
    wdtcnt_gap_set(h, 0x7d0);
    printf("-- [img_mode_init(0) COMPLETE] --\n");
}

/* fw9366_img_scan_start(), traced at 0x15c4ab: calls img_mode_init(0)
 * again, wm_switch(3), polls wm_get() up to 10 times waiting for 0x54,
 * then sets bit 0 of 0x1800 and sleeps 1ms. */
static void img_scan_start(libusb_device_handle *h)
{
    img_mode_init_0(h);
    printf("-- img_scan_start: wm_switch(3) --\n");
    wm_switch(h, 3);
    unsigned char wm = 0;
    int tries = 10;
    do {
        wm_get(h, &wm);
        printf("-- img_scan_start: wm_get() = 0x%02x --\n", wm);
        if (wm == 0x54) break;
        tries--;
    } while (tries != 0);
    printf("-- img_scan_start: 0x1800 set bit0 --\n");
    unsigned short v = 0;
    sram_read(h, 0x1800, &v);
    v = sram_bits_set(v, 0, 0, 1);
    sram_write(h, 0x1800, v);
    usleep(1 * 1000);
}

/* fw9366_img_scan_end(), traced at 0x15c5bf: intflag_clear(FW9366_INT_INDEX[5]=0x20). */
static void img_scan_end(libusb_device_handle *h)
{
    printf("-- img_scan_end: intflag_clear(0x20) --\n");
    sram_write(h, 0x1a84, 0x20); /* intflag_clear(mask) = sram_write(0x1a84, mask) */
}

/* fw9366_image_read(out_buf, param), traced at 0x166a5b: param clamped to
 * [0,5] (0 mapped to 1), total_len = param*5*2048, read in chunks of up
 * to 0x2800(10240) bytes via fifo_read(0x1a05, ...) (fixed FIFO address
 * every chunk -- a streaming read port, not a growing memory range),
 * each chunk copied into out_buf sequentially. out_buf must be large
 * enough for total_len bytes (up to 10240 for param<=1). */
static void image_read(libusb_device_handle *h, unsigned char *out_buf, int param)
{
    if (param > 5) param = 5;
    if (param == 0) param = 1;
    unsigned int total_len = (unsigned int)param * 5u * 2048u;
    unsigned int offset = 0;
    printf("-- image_read: param=%d total_len=%u --\n", param, total_len);
    while (total_len > 0) {
        unsigned int chunk = (total_len > 10240) ? 10240 : total_len;
        fifo_read(h, 0x1a05, out_buf + offset, chunk);
        offset += chunk;
        total_len -= chunk;
    }
}

/* fw9366_img_data_get(out_buf, param), traced at 0x15c64d. */
static void img_data_get(libusb_device_handle *h, unsigned char *out_buf, int param)
{
    img_scan_start(h);
    image_read(h, out_buf, param);
    img_scan_end(h);
}

/* fw9366_Set_Scan_Rate_Default(), traced at 0x155df0: same pattern as
 * Set_Scan_Rate_2M -- three live read-modify-write ops, no host-state
 * dependency. */
static int set_scan_rate_default(libusb_device_handle *h)
{
    unsigned short v;
    printf(" set_scan_rate_default(): 0x1806\n");
    sram_read(h, 0x1806, &v);
    v = sram_bits_set(v, 13, 7, 0x13);
    sram_write(h, 0x1806, v);

    printf(" set_scan_rate_default(): 0x180a\n");
    sram_read(h, 0x180a, &v);
    v = sram_bits_set(v, 13, 7, 0x13);
    v = sram_bits_set(v, 6, 0, 7);
    sram_write(h, 0x180a, v);

    printf(" set_scan_rate_default(): 0x180b\n");
    sram_read(h, 0x180b, &v);
    v = sram_bits_set(v, 13, 7, 9);
    v = sram_bits_set(v, 6, 0, 0x11);
    sram_write(h, 0x180b, v);
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

    /* --- fw9366_img_mode_init(0) -- OPENING traced (0x15b8a9, 3074 bytes
     * total, ~13% covered). Called here because REG9366[0x77]==0 (confirmed
     * via fw9366_init_flag). Resolved so far:
     *   idle_enter()   -- called again internally; harmless to repeat live.
     *   sram_write(0x1801, sram_bits_set(0xfc80, hi=6,lo=0, new=REG9366[0x87]))
     *     = sram_write(0x1801, 0xfcb6)  -- REG9366[0x87]=0x36 confirmed via
     *       fw9366_init_flag (unconditional write, already traced).
     *     NOTE: this happens BEFORE fdt_mode_init's own 0x1801 write
     *     (0xfc9b) in the real sequence -- two writes to the same address,
     *     not one; both must be sent in order for wire-level fidelity, even
     *     though the second overwrites the first.
     *   FW9366_LAST_DAC = REG9366[0x87]  -- host-side only, no wire effect.
     *   if (param==0): sram_write(0x1800, 0x4ffe)   -- FIXED constant, our
     *       case (param=0), no host-state dependency.
     *   (param!=0 branch not relevant -- real call always uses param=0 here)
     *
     * CONTINUED (both branches converge here): there is a SECOND
     * fw9366_context[0xfc] gate at this point (0x15bcec), checking against
     * 0xa3 this time (a state value distinct from fdt_mode_init's 0xa0/
     * 0xa1/0xa2). At this exact point in the real sequence, +0xfc is still
     * 0xa0 (fdt_mode_init's own write to 0xa1 happens AFTER img_mode_init
     * returns), so 0xa0 != 0xa3 and the gate is confirmed passed through to
     * the main block, which then sets +0xfc=0xa3 itself:
     *   fw9366_context[0xfc] = 0xa3   -- host-side only, no wire effect
     *   sram_write(0x1804, sram_bits_set(sram_bits_set(sram_bits_set(0x7c0,
     *       hi=1,lo=0,new=Fw9366_cfg[0xa]), hi=3,lo=3,new=1 [since
     *       Fw9366_cfg[0xa]=2>1]), hi=0xd,lo=0xd,new=1))
     *     = sram_write(0x1804, 0x27ca)   -- Fw9366_cfg[0xa]=0x02 confirmed via cfg_init
     *   fw9366_Set_Scan_Rate_2M()   -- three live read-modify-write ops,
     *       no host-state dependency (reads real current hardware value
     *       each time) -- see set_scan_rate_2m() below.
     *   sram_write(0x1807, sram_bits_set(sram_bits_set(1, hi=0xd,lo=5,
     *       new=Fw9366_cfg[0xc]-1), hi=4,lo=4,new=0 [since Fw9366_cfg[2]!=0]))
     *     = sram_write(0x1807, 0x18e1)   -- Fw9366_cfg[0xc]=0xc8 confirmed
     *       (this session's earlier smic_flag=0 live measurement), Fw9366_cfg[2]=1 confirmed
     *
     * Now factored into img_mode_init_0(), defined above main() -- also
     * reused by img_scan_start(). */
    img_mode_init_0(h);
    printf("\n");

    printf("-- sram_write(0x1801, 0xfc9b) --\n");
    sram_write(h, 0x1801, 0xfc9b);
    printf("-- sram_write(0x180c, 0x0000) [CORRECTED: AUTO_DAC_PRO_FLAG=1 branch, not the ==0 branch] --\n");
    sram_write(h, 0x180c, 0x0000);
    printf("-- sram_write(0x1881, 0x0f0c) --\n");
    sram_write(h, 0x1881, 0x0f0c);
    printf("-- [fdt_mode_init: fw9366_context[0xfc] = 0xa1 -- host-side only, no wire effect] --\n\n");

    /* --- fdt_mode_init CONTINUED after context[0xfc]=0xa1 (0x156853-0x156c26) ---
     * All three below are FURTHER writes to addresses already written by
     * img_mode_init(0) above -- same "multiple writes to the same address"
     * pattern already caught twice this session; preserved in order, not
     * collapsed.
     *   sram_write(0x1800, sram_bits_set(sram_bits_set(0,hi=0xa,lo=1,new=0x3ff),
     *       hi=0xe,lo=0xb,new=0))   -- Fw9366_cfg[2]!=0 branch (confirmed always true)
     *     = sram_write(0x1800, 0x07fe)   -- 2nd write to 0x1800 (img_mode_init wrote 0x4ffe)
     *   sram_write(0x1804, sram_bits_set(0x27c8, hi=2,lo=0,new=Fw9366_cfg[1]-1))
     *     = sram_write(0x1804, 0x27c8)   -- Fw9366_cfg[1]=0x01 confirmed via cfg_init;
     *       3rd write to 0x1804 total (img_mode_init wrote 0x27ca -- differs by ONE BIT,
     *       easy to miss if collapsed)
     *   if (Fw9366_cfg[2]!=0): sram_write(0x1807, 0x1671)   -- FIXED constant, no
     *       bits_set computation at all; 2nd write to 0x1807 (img_mode_init wrote 0x18e1) */
    printf("-- fdt_mode_init: sram_write(0x1800, 0x07fe) [2nd write to this addr] --\n");
    sram_write(h, 0x1800, 0x07fe);
    printf("-- fdt_mode_init: sram_write(0x1804, 0x27c8) [3rd write to this addr, differs by 1 bit from img_mode_init's 0x27ca] --\n");
    sram_write(h, 0x1804, 0x27c8);
    printf("-- fdt_mode_init: sram_write(0x1807, 0x1671) [2nd write to this addr, fixed constant] --\n");
    sram_write(h, 0x1807, 0x1671);

    /* --- fdt_mode_init CONTINUED (0x156c30-0x157082+) ---
     *   sram_write(0x1808, sram_bits_set(sram_bits_set(0x800,hi=7,lo=0,new=1),
     *       hi=0xa,lo=8,new=0))   -- Fw9366_cfg[2]!=0 branch (confirmed always true)
     *     = sram_write(0x1808, 0x0801)
     *   sram_write(0x1887, sram_bits_set(0,hi=1,lo=0,new=5))   -- Fw9366_cfg[2]!=0 branch
     *     = sram_write(0x1887, 0x0001)   -- 2nd write to 0x1887 (img_mode_init wrote 2)
     *   fw9366_Set_Scan_Rate_Default()   -- live RMW, no host-state dependency (see below)
     *   if (REG9366[0x78] == 1): [FALSE -- REG9366[0x78]=0 confirmed via init_flag, so we
     *       take the real-work path, not the dead-end log-and-exit branch]
     *   v = sram_read(0x1805); v = bits_set(v,4,4,0); sram_write(0x1805, v)
     *     -- live RMW; ANOTHER write to 0x1805 (already written by img_mode_init's tail) */
    printf("-- fdt_mode_init: sram_write(0x1808, 0x0801) --\n");
    sram_write(h, 0x1808, 0x0801);
    printf("-- fdt_mode_init: sram_write(0x1887, 0x0001) [2nd write to this addr, img_mode_init wrote 2] --\n");
    sram_write(h, 0x1887, 0x0001);
    printf("-- fdt_mode_init: set_scan_rate_default() --\n");
    set_scan_rate_default(h);
    printf("-- fdt_mode_init: 0x1805 live read-modify-write (clear bit4) [another write to this addr] --\n");
    unsigned short v_1805 = 0;
    sram_read(h, 0x1805, &v_1805);
    v_1805 = sram_bits_set(v_1805, 4, 4, 0);
    sram_write(h, 0x1805, v_1805);

    /* --- fdt_mode_init CONTINUED (0x1570cd-0x157360) ---
     * IMPORTANT: 0x180d's base value is NOT fresh -- it's a compiler
     * register-reuse artifact that chains directly from 0x1805's
     * just-computed value above (confirmed via careful disassembly
     * reading: no intervening read/reset of that local before use here).
     * 0x1888 is a fresh, independent read. sfr_write(0x9a,0x5a) is a
     * direct fixed-value SFR write (no SRAM involved). */
    printf("-- fdt_mode_init: sram_write(0x180d, ...) [CHAINED from 0x1805's value, not fresh] --\n");
    sram_write(h, 0x180d, sram_bits_set(v_1805, 9, 0, 0x384));
    printf("-- fdt_mode_init: 0x1888 live read-modify-write (fresh) --\n");
    {
        unsigned short v = 0;
        sram_read(h, 0x1888, &v);
        v = sram_bits_set(v, 9, 2, 0);
        sram_write(h, 0x1888, v);
    }
    printf("-- fdt_mode_init: sfr_write(0x9a, 0x5a) --\n");
    sfr_write(h, 0x9a, 0x5a);

    /* --- fdt_mode_init: 3-iteration loop (0x157373-0x157c19), resolved
     * via radare2 control-flow analysis after linear disassembly reading
     * left real ambiguity (address 0xC0 appeared to be computed a second
     * time, suggesting a loop rather than a one-shot block). Confirmed via
     * basic-block graph: loop increments the counter by 9 (`add
     * BYTE[rbp-1],0x9`), bounded by `<=0x13(19)`, starting at 0 on our
     * path (Fw9366_cfg[2]!=0) -- exactly 3 iterations: i = 0, 9, 18.
     * Fixed values used every iteration (computed once before the loop,
     * NOT per-iteration): 0x2224 split into bitfield_hi=0x2224>>3=0x444
     * and bitfield_lo=0x2224&7=4; separately, a fixed count variable
     * (4) minus 1 = 3.
     * Per iteration (3 registers, all live read-modify-write):
     *   addr1 = 0xbf+i: v=sram_read; v=bits_set(v,12,0,0x444); sram_write
     *   addr2 = 0xc0+i: v=sram_read; v=bits_set(v,5,3,4); v=bits_set(v,1,0,1); sram_write
     *   addr3 = 0xc1+i: v=sram_read; v=bits_set(v,7,6,3); sram_write */
    printf("-- fdt_mode_init: 3-iteration loop (0xbf/0xc0/0xc1 + i, i=0,9,18) --\n");
    {
        int offsets[3] = { 0, 9, 18 };
        for (int k = 0; k < 3; k++) {
            int i = offsets[k];
            unsigned short v;
            printf(" -- iteration i=%d --\n", i);
            sram_read(h, (unsigned short)(0xbf + i), &v);
            v = sram_bits_set(v, 12, 0, 0x444);
            sram_write(h, (unsigned short)(0xbf + i), v);

            sram_read(h, (unsigned short)(0xc0 + i), &v);
            v = sram_bits_set(v, 5, 3, 4);
            v = sram_bits_set(v, 1, 0, 1);
            sram_write(h, (unsigned short)(0xc0 + i), v);

            sram_read(h, (unsigned short)(0xc1 + i), &v);
            v = sram_bits_set(v, 7, 6, 3);
            sram_write(h, (unsigned short)(0xc1 + i), v);
        }
    }
    /* --- fdt_mode_init FINAL SECTION (0x158080-0x158223) --- function ends
     * here; this completes fdt_mode_init 100%. */
    printf("-- fdt_mode_init: sram_write(0x1a8a, 0xff) --\n");
    sram_write(h, 0x1a8a, 0xff);
    printf("-- fdt_mode_init: intflag_mask(3) --\n");
    intflag_mask(h, 3);
    printf("-- [fdt_mode_init: REG9366[0x78]=1 -- host-side only, no wire effect] --\n");
    printf("\n== fdt_mode_init COMPLETE -- fully traced, 100%% ==\n");

    /* --- fw9366_fdt_get_a_frame_data() -- fully traced (200 bytes), tested
     * here. Called next in fdt_base_Stable_Update's real sequence, after
     * fdt_manual_start returns. Uses a NEW primitive,
     * sram_read_bulk_withecc, the first variable-length bulk read this
     * session (vs. single-register reads so far). */
    printf("\n== fw9366_fdt_get_a_frame_data() -- fully traced, 100%% ==\n");
    unsigned char frame_buf[8] = { 0 };
    fdt_get_a_frame_data(h, frame_buf);
    hexdump("  frame_buf after byte-swap", frame_buf, sizeof(frame_buf));

    /* --- fw9392_fdt_base_fail_check() -- pure local, no I/O, fully traced.
     * Tested here directly against the real frame_buf just captured above
     * -- a decisive test of whether that all-zero capture is usable
     * calibration data or not, using the real driver's own validity logic. */
    printf("\n== fw9392_fdt_base_fail_check() against the captured frame_buf ==\n");
    int fail_result = fdt_base_fail_check(frame_buf);
    printf("  fdt_base_fail_check() returned: %d (%s)\n", fail_result,
           fail_result == 0 ? "PASS" : "FAIL");

    /* --- fw9366_fdt_base_Min_Updata() -- pure local, no I/O, fully traced.
     * Run against the same captured frame_buf for a real, verified result
     * (not just algorithm-level verification). */
    printf("\n== fw9366_fdt_base_Min_Updata() against the captured frame_buf ==\n");
    unsigned short crc1 = 0, crc2 = 0;
    fdt_base_min_updata(frame_buf, &crc1, &crc2);
    printf("  crc1 (-> REG9366+0xca) = 0x%04x\n", crc1);
    printf("  crc2 (-> REG9366+0xb6) = 0x%04x\n", crc2);

    /* --- fw9366_img_data_get(param=0) -- REAL IMAGE CAPTURE, fully traced.
     * This is the actual raw fingerprint image read path (up to 10240
     * bytes for param=0->1), distinct from the small 8-byte calibration
     * "frame data" tested earlier. First test run with no finger placed,
     * to establish a baseline before any touch-based comparison. */
    printf("\n== fw9366_img_data_get(param=0) -- REAL IMAGE CAPTURE ==\n");
    static unsigned char image_buf[10240];
    img_data_get(h, image_buf, 0);
    printf("== image capture complete ==\n");

    const char *out_path = getenv("IMAGE_OUT_PATH");
    if (out_path == NULL) out_path = "/tmp/last_capture.raw";
    FILE *f = fopen(out_path, "wb");
    if (f) {
        fwrite(image_buf, 1, sizeof(image_buf), f);
        fclose(f);
        printf("== saved full %zu-byte capture to %s ==\n", sizeof(image_buf), out_path);
    } else {
        printf("== FAILED to save capture to %s ==\n", out_path);
    }

    /* Img_Get_Avg_Middle() -- pure local, tested here against the real
     * capture just taken. */
    int avg_middle = img_get_avg_middle(image_buf, (int)sizeof(image_buf));
    printf("== Img_Get_Avg_Middle() = %d ==\n", avg_middle);

    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
