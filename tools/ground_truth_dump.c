/*
 * DIAGNOSTIC-ONLY tool. Calls the REAL proprietary FocalTech .so's
 * internal FtGetTemplate() directly (via a computed load-bias + static
 * file offset, since it is a LOCAL symbol not in .dynsym) to dump real
 * ground-truth keypoints/descriptors for comparison against this
 * project's own from-scratch reimplementation (focal_sift.c).
 *
 * NEVER shipped, NEVER used at runtime for end users -- authorized for
 * this one-time diagnostic purpose only, per the session decision logged
 * in research/PROTOCOL.md ("DECISION: for THIS diagnostic purpose only").
 * The actual driver continues to use the independent, zero-dependency
 * reimplementation in focal_match.c/focal_sift.c/focal_verify.c.
 *
 * Build: gcc -O0 -g -Wall ground_truth_dump.c -o ground_truth_dump -ldl
 * Usage: ./ground_truth_dump <path-to-libfprint-2.so> <capture.raw> <out.txt>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef float    FP32;

/* Static file offsets, from `nm`/`readelf` on the target .so (see
 * research/PROTOCOL.md for how these were located). */
#define OFF_FtGetTemplate         0xbfea0
#define OFF_fp_device_verify      0x15a30  /* exported anchor for load bias */
#define OFF_gFocalTempupdateInfor 0x30c5650 /* uninitialized global ptr --
                                                must point to a real
                                                ST_FocalImageInfor or
                                                FtGetTemplate segfaults
                                                writing to ->imageArea */

/* ST_Feature -- CONFIRMED layout (research/PROTOCOL.md, DWARF-derived) */
typedef struct {
    FP32 x, y, ori;
    UINT32 bDescri[8];
} ST_Feature;

/* ST_FocalTemplate -- CONFIRMED layout (520 bytes total, DWARF-derived) */
typedef struct {
    ST_Feature *pTemplateFeature;      /* 0 */
    UINT8 *templateBinDiscr;           /* 8 */
    UINT8 *templatePixValid;           /* 16 */
    UINT32 headerSize;                 /* 24 */
    UINT32 featBufSize;                /* 28 */
    UINT32 binBufSize;                 /* 32 */
    UINT32 maskBufSize;                /* 36 */
    UINT32 templateSize;               /* 40 */
    UINT32 templateBinDiscrLen;        /* 44 */
    UINT16 templateExtendArea;         /* 48 */
    UINT16 subtemplatesPairIndex;      /* 50 */
    FP32 subtemplatePairHmatrix[10];   /* 52 */
    UINT8 templateCoinHmatrix[384];    /* 92 -- opaque here, not needed */
    UINT8 nFeatureNum[2];              /* 476 */
    UINT8 templatePartsNum;            /* 478 */
    UINT8 templateArea;                /* 479 */
    UINT8 templateQuality;             /* 480 */
    UINT8 templateCondition;           /* 481 */
    UINT8 templateContrast;            /* 482 */
    UINT8 tempReplaceFlg;              /* 483 */
    UINT8 keepByte2, keepByte3, keepByte4, keepByte5; /* 484-487 */
    UINT8 templateCoinFlag[25];        /* 488 */
    UINT8 pad[7];
} ST_FocalTemplate;

typedef struct {
    UINT8 quality, area, cond, contrast, reser;
} ST_FocalSensorImageInfo;

typedef int (*FtGetTemplate_fn)(UINT8 *image, ST_FocalTemplate *tpl, ST_FocalSensorImageInfo *info);

static int cmp_u16(const void *a, const void *b)
{
    return (int)(*(const unsigned short *)a) - (int)(*(const unsigned short *)b);
}

static unsigned char *load_raw_be16_as_u8(const char *path, int npix)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    unsigned char *buf = malloc((size_t)npix * 2);
    if (fread(buf, 1, (size_t)npix * 2, f) != (size_t)npix * 2) { fprintf(stderr, "short read\n"); exit(1); }
    fclose(f);
    unsigned short *px = malloc((size_t)npix * sizeof(unsigned short));
    int i;
    for (i = 0; i < npix; i++) px[i] = (unsigned short)((buf[i * 2] << 8) | buf[i * 2 + 1]);
    free(buf);
    unsigned short *sorted = malloc((size_t)npix * sizeof(unsigned short));
    memcpy(sorted, px, (size_t)npix * sizeof(unsigned short));
    qsort(sorted, npix, sizeof(unsigned short), cmp_u16);
    unsigned short lo = sorted[(int)(npix * 0.01)], hi = sorted[(int)(npix * 0.99)];
    free(sorted);
    if (hi <= lo) hi = lo + 1;
    unsigned char *out = malloc((size_t)npix);
    for (i = 0; i < npix; i++) {
        int v = px[i];
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        out[i] = (unsigned char)(((v - lo) * 255) / (hi - lo));
    }
    free(px);
    return out;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <libfprint-2.so path> <capture.raw> <out.txt>\n", argv[0]);
        return 1;
    }
    const char *soPath = argv[1], *rawPath = argv[2], *outPath = argv[3];
    const int rows = 80, cols = 64;

    void *handle = dlopen(soPath, RTLD_LAZY);
    if (!handle) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }

    void *anchor = dlsym(handle, "fp_device_verify");
    if (!anchor) { fprintf(stderr, "dlsym(fp_device_verify) failed: %s\n", dlerror()); return 1; }

    uintptr_t bias = (uintptr_t)anchor - OFF_fp_device_verify;
    FtGetTemplate_fn FtGetTemplate = (FtGetTemplate_fn)(bias + OFF_FtGetTemplate);

    fprintf(stderr, "anchor fp_device_verify @ %p, computed bias=0x%lx, FtGetTemplate @ %p\n",
            anchor, (unsigned long)bias, (void *)FtGetTemplate);

    /* Initialize the uninitialized global gFocalTempupdateInfor pointer
     * (see OFF_gFocalTempupdateInfor comment) -- allocate generously (well
     * beyond the real ST_FocalImageInfor's ~530 bytes) and zero it. */
    void **gFocalTempupdateInfor_slot = (void **)(bias + OFF_gFocalTempupdateInfor);
    void *dummyUpdateInfor = calloc(1, 4096);
    *gFocalTempupdateInfor_slot = dummyUpdateInfor;
    fprintf(stderr, "gFocalTempupdateInfor @ %p set to %p\n",
            (void *)gFocalTempupdateInfor_slot, dummyUpdateInfor);

    unsigned char *img_tight = load_raw_be16_as_u8(rawPath, rows * cols);
    /* MAJOR FINDING (research/PROTOCOL.md): gSensorInfor.sensorCols/Rows
     * (a real compiled-in .data global) = 96x96, not our sensor's actual
     * native 64x80 wire resolution. The crash's memcpy length (96) matches
     * this exactly, strongly suggesting FtGetTemplate expects pImageBuff
     * to already be laid out as a 96x96 canvas with our real 64x80 data
     * embedded in it -- testing the natural hypothesis: centered,
     * zero-padded ((96-64)/2=16 cols each side, (96-80)/2=8 rows each side). */
    const int canvasDim = 96;
    unsigned char *img = calloc(1, (size_t)canvasDim * canvasDim + 32768 /* extra safety margin */);
    int padRows = (canvasDim - rows) / 2, padCols = (canvasDim - cols) / 2;
    int rr;
    for (rr = 0; rr < rows; rr++)
        memcpy(img + (rr + padRows) * canvasDim + padCols, img_tight + rr * cols, (size_t)cols);
    free(img_tight);

    ST_FocalTemplate tpl;
    memset(&tpl, 0, sizeof(tpl));
    ST_FocalSensorImageInfo info;
    memset(&info, 0, sizeof(info));

    int ret = FtGetTemplate(img, &tpl, &info);

    FILE *out = fopen(outPath, "w");
    fprintf(out, "# DIAGNOSTIC-ONLY ground truth dump from the real proprietary .so\n");
    fprintf(out, "# source_image=%s\n", rawPath);
    fprintf(out, "# FtGetTemplate return code = %d\n", ret);
    fprintf(out, "# info: quality=%d area=%d cond=%d contrast=%d\n",
            info.quality, info.area, info.cond, info.contrast);
    fprintf(out, "# nFeatureNum[0]=%d nFeatureNum[1]=%d\n", tpl.nFeatureNum[0], tpl.nFeatureNum[1]);
    fprintf(out, "# featBufSize=%u templateSize=%u templateArea=%d templateQuality=%d templateCondition=%d templateContrast=%d\n",
            tpl.featBufSize, tpl.templateSize, tpl.templateArea, tpl.templateQuality,
            tpl.templateCondition, tpl.templateContrast);
    fprintf(out, "# pTemplateFeature=%p\n", (void *)tpl.pTemplateFeature);

    if (ret == 0 && tpl.pTemplateFeature) {
        int maxByBuf = tpl.featBufSize ? (int)(tpl.featBufSize / sizeof(ST_Feature)) : 0;
        int n = tpl.nFeatureNum[0] + tpl.nFeatureNum[1];
        if (n <= 0 || n > 500) n = maxByBuf > 0 && maxByBuf <= 500 ? maxByBuf : 200;
        fprintf(stderr, "dumping up to %d features (maxByBuf=%d)\n", n, maxByBuf);
        fprintf(out, "# dumping %d features (x y ori bDescri[0..7] as 8 hex words)\n", n);
        int i;
        for (i = 0; i < n; i++) {
            ST_Feature *f = &tpl.pTemplateFeature[i];
            fprintf(out, "%d %.6f %.6f %.6f", i, f->x, f->y, f->ori);
            int k;
            for (k = 0; k < 8; k++) fprintf(out, " %08x", f->bDescri[k]);
            fprintf(out, "\n");
        }
    }
    fclose(out);
    free(img);
    printf("wrote %s (return=%d)\n", outPath, ret);
    return 0;
}
