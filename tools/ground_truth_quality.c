/*
 * DIAGNOSTIC-ONLY tool. Calls the REAL proprietary FocalTech .so's
 * FtGetImageQuality() (a capture-time quality-gating function -- see
 * research/PROTOCOL.md "Windows-vs-Linux discrepancy investigation,
 * STEP 2") against a raw capture and prints the real
 * ST_FocalSensorImageInfo{quality,area,cond,contrast} result.
 *
 * Sets gSensorInfor's cols/rows via the real FtSetSensorColRow(64,80)
 * before calling, since FtGetImageQuality reads sensor dimensions from
 * that global to dispatch into FtImgQuality.
 *
 * NEVER shipped, NEVER used at runtime -- diagnostic-only.
 *
 * Build: gcc -O0 -g -Wall ground_truth_quality.c -o ground_truth_quality -ldl
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

typedef uint8_t  UINT8;
typedef int16_t  SINT16;
typedef uint16_t UINT16;

#define OFF_FtGetImageQuality   0xb82f0
#define OFF_FtSetSensorColRow   0xb8c30
#define OFF_fp_device_verify    0x15a30
#define OFF_gFocalTempupdateInfor 0x30c5650

typedef struct { UINT8 quality, area, cond, contrast, reser; } ST_FocalSensorImageInfo;

typedef short (*FtGetImageQuality_fn)(UINT8 *, ST_FocalSensorImageInfo *);
typedef void  (*FtSetSensorColRow_fn)(SINT16, SINT16);

static int cmp_u16(const void *a, const void *b) { return (int)(*(const unsigned short *)a) - (int)(*(const unsigned short *)b); }
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
    for (i = 0; i < npix; i++) { int v=px[i]; if(v<lo)v=lo; if(v>hi)v=hi; out[i]=(unsigned char)(((v-lo)*255)/(hi-lo)); }
    free(px);
    return out;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <so_path> <capture.raw> [more.raw ...]\n", argv[0]); return 1; }
    const char *soPath = argv[1];
    const int rows = 80, cols = 64;

    void *handle = dlopen(soPath, RTLD_LAZY);
    if (!handle) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    void *anchor = dlsym(handle, "fp_device_verify");
    uintptr_t bias = (uintptr_t)anchor - OFF_fp_device_verify;

    FtSetSensorColRow_fn setColRow = (FtSetSensorColRow_fn)(bias + OFF_FtSetSensorColRow);
    FtGetImageQuality_fn getQuality = (FtGetImageQuality_fn)(bias + OFF_FtGetImageQuality);

    void **gFocalTempupdateInfor_slot = (void **)(bias + OFF_gFocalTempupdateInfor);
    *gFocalTempupdateInfor_slot = calloc(1, 4096);

    setColRow((SINT16)cols, (SINT16)rows);

    int i;
    for (i = 2; i < argc; i++) {
        const char *path = argv[i];
        unsigned char *img = load_raw_be16_as_u8(path, rows * cols);
        ST_FocalSensorImageInfo info; memset(&info, 0, sizeof(info));
        short ret = getQuality(img, &info);
        printf("%s: ret=%d quality=%u area=%u cond=%u contrast=%u\n",
               path, ret, info.quality, info.area, info.cond, info.contrast);
        free(img);
    }
    return 0;
}
