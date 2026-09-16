/*
 * Standalone test: run libfprint's own bundled NBIS pipeline (MINDTCT minutiae
 * extraction + Bozorth3 matching) directly against our real FT9366 captures,
 * BEFORE committing to writing a full FpImageDevice driver.
 *
 * Rationale: whole-image NCC correlation (calibrated + rotation/translation
 * aligned) failed to separate same-finger from different-finger captures
 * across 45 real pairwise comparisons (see research/PROTOCOL.md). Per the
 * session's own pre-set fallback, the next step is minutiae-based matching --
 * but libfprint already bundles a full, production NBIS pipeline
 * (mindtct + bozorth3) used automatically by every FpImageDevice driver.
 * Reuse over reinvention: this test calls get_minutiae()/bozorth_to_gallery()
 * exactly the way libfprint/fp-image.c and libfprint/fpi-print.c do,
 * directly on our raw captures, to find out empirically whether this
 * sensor's tiny 64x80 native resolution is even sufficient for MINDTCT to
 * find real minutiae -- rather than assuming it will work and building a
 * whole driver first.
 *
 * Build (from ~/Desktop/libfprint, meson build already configured in ./build):
 *   gcc -O0 -g -Wall nbis_minutiae_test.c \
 *     -I../libfprint/libfprint/nbis/include \
 *     -I../libfprint/libfprint/nbis/libfprint-include \
 *     ../libfprint/build/libfprint/libnbis.a \
 *     -lm -lglib-2.0 -I/usr/include/glib-2.0 -I/usr/lib/glib-2.0/include \
 *     -o nbis_minutiae_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <lfs.h>
#include <bozorth.h>

#define SENSOR_W 64
#define SENSOR_H 80
#define SENSOR_NPIX (SENSOR_W * SENSOR_H)

/* 500 DPI is the de-facto standard assumption for small fingerprint area
 * sensors (see libfprint/drivers/secugen.c: SECUGEN_PPMM = 19.685 = 500dpi
 * / 25.4mm). We do NOT have a confirmed physical sensor size for the
 * FT9366, so this is an assumption, not a measured fact -- flagged as such. */
#define ASSUMED_PPMM 19.685

static int cmp_u16(const void *a, const void *b)
{
    return (int)(*(const unsigned short *)a) - (int)(*(const unsigned short *)b);
}

static int load_raw_be16_as_u8(const char *path, unsigned char *out8, int npix)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    unsigned char *buf = malloc(npix * 2);
    size_t n = fread(buf, 1, npix * 2, f);
    fclose(f);
    if ((int)n != npix * 2) {
        fprintf(stderr, "%s: expected %d bytes, got %zu\n", path, npix * 2, n);
        free(buf);
        return -1;
    }

    unsigned short *px = malloc(npix * sizeof(unsigned short));
    int i;
    for (i = 0; i < npix; i++)
        px[i] = (unsigned short)((buf[i * 2] << 8) | buf[i * 2 + 1]);
    free(buf);

    /* Robust min/max (1st/99th percentile) normalization to 8-bit, since raw
     * pixel values are real ADC counts (observed range ~0-2022 on this
     * sensor, not a full 0-65535 16-bit range). */
    unsigned short *sorted = malloc(npix * sizeof(unsigned short));
    memcpy(sorted, px, npix * sizeof(unsigned short));
    qsort(sorted, npix, sizeof(unsigned short), cmp_u16);
    unsigned short lo = sorted[(int)(npix * 0.01)];
    unsigned short hi = sorted[(int)(npix * 0.99)];
    free(sorted);
    if (hi <= lo) hi = lo + 1;

    for (i = 0; i < npix; i++) {
        int v = px[i];
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        /* MINDTCT expects standard grayscale image convention: ridges are
         * typically darker. We don't know this sensor's polarity for
         * certain, so just do a plain linear scale here (no inversion);
         * polarity can be flipped later if minutiae quality looks better
         * inverted. */
        out8[i] = (unsigned char)(((v - lo) * 255) / (hi - lo));
    }
    free(px);
    return 0;
}

static void print_minutiae(const char *label, MINUTIAE *m)
{
    printf("--- %s: %d minutiae found ---\n", label, m->num);
    int i;
    for (i = 0; i < m->num && i < 40; i++) {
        struct fp_minutia *mm = m->list[i];
        printf("  [%2d] x=%3d y=%3d dir=%3d reliability=%.3f type=%d\n",
               i, mm->x, mm->y, mm->direction, mm->reliability, mm->type);
    }
    if (m->num > 40) printf("  ... (%d more)\n", m->num - 40);
}

/* Mirrors libfprint/fpi-print.c minutiae_to_xyt() exactly, so our bozorth3
 * match results are representative of what a real driver would produce. */
static void minutiae_to_xyt_local(MINUTIAE *minutiae, int bwidth, int bheight,
                                   struct xyt_struct *xyt)
{
    int i;
    struct minutiae_struct c[MAX_FILE_MINUTIAE];
    int nmin = minutiae->num < MAX_BOZORTH_MINUTIAE ? minutiae->num : MAX_BOZORTH_MINUTIAE;

    for (i = 0; i < nmin; i++) {
        struct fp_minutia *minutia = minutiae->list[i];
        lfs2nist_minutia_XYT(&c[i].col[0], &c[i].col[1], &c[i].col[2],
                              minutia, bwidth, bheight);
        c[i].col[3] = sround(minutia->reliability * 100.0);
        if (c[i].col[2] > 180) c[i].col[2] -= 360;
    }

    qsort((void *)c, (size_t)nmin, sizeof(struct minutiae_struct), sort_x_y);

    for (i = 0; i < nmin; i++) {
        xyt->xcol[i] = c[i].col[0];
        xyt->ycol[i] = c[i].col[1];
        xyt->thetacol[i] = c[i].col[2];
    }
    xyt->nrows = nmin;
}

static int extract(const char *path, MINUTIAE **out_minutiae, struct xyt_struct *out_xyt)
{
    unsigned char img8[SENSOR_NPIX];
    if (load_raw_be16_as_u8(path, img8, SENSOR_NPIX) != 0)
        return -1;

    int *direction_map = NULL, *low_contrast_map = NULL, *low_flow_map = NULL;
    int *high_curve_map = NULL, *quality_map = NULL;
    int map_w, map_h, bw, bh, bd;
    unsigned char *bdata = NULL;
    LFSPARMS lfsparms = g_lfsparms_V2;
    lfsparms.remove_perimeter_pts = FALSE;

    int r = get_minutiae(out_minutiae, &quality_map, &direction_map,
                          &low_contrast_map, &low_flow_map, &high_curve_map,
                          &map_w, &map_h, &bdata, &bw, &bh, &bd,
                          img8, SENSOR_W, SENSOR_H, 8, ASSUMED_PPMM, &lfsparms);
    if (r) {
        fprintf(stderr, "%s: get_minutiae failed, code %d\n", path, r);
        return r;
    }

    print_minutiae(path, *out_minutiae);
    minutiae_to_xyt_local(*out_minutiae, bw, bh, out_xyt);

    free(direction_map); free(low_contrast_map); free(low_flow_map);
    free(high_curve_map); free(quality_map); free(bdata);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s file1.raw file2.raw [file3.raw ...]\n", argv[0]);
        fprintf(stderr, "  extracts minutiae from each file via real MINDTCT, then\n");
        fprintf(stderr, "  runs bozorth3 matching on every pair, printing raw scores.\n");
        return 1;
    }

    int n = argc - 1;
    MINUTIAE **minutiae = calloc(n, sizeof(MINUTIAE *));
    struct xyt_struct *xyt = calloc(n, sizeof(struct xyt_struct));
    int *ok = calloc(n, sizeof(int));

    int i, j;
    for (i = 0; i < n; i++) {
        ok[i] = (extract(argv[i + 1], &minutiae[i], &xyt[i]) == 0);
    }

    printf("\n=== bozorth3 pairwise scores (higher = more similar; typical libfprint\n");
    printf("    bz3_threshold default is 40) ===\n");
    for (i = 0; i < n; i++) {
        if (!ok[i]) continue;
        for (j = i + 1; j < n; j++) {
            if (!ok[j]) continue;
            int probe_len = bozorth_probe_init(&xyt[i]);
            int score = bozorth_to_gallery(probe_len, &xyt[i], &xyt[j]);
            printf("  %s vs %s: score=%d\n", argv[i + 1], argv[j + 1], score);
        }
    }

    return 0;
}
