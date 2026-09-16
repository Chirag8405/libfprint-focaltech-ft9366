/*
 * DIAGNOSTIC-ONLY tool. Calls the REAL proprietary FocalTech .so's
 * FtGetTemplateForEnroll() directly -- a DISTINCT function from
 * FtGetTemplate, found while investigating why this sensor works
 * correctly on Windows but this project's testing (which never routed
 * through either function's real preprocessing chain) does not separate
 * same/different-finger identity. See research/PROTOCOL.md.
 *
 * NEVER shipped, NEVER used at runtime -- diagnostic-only.
 *
 * Build: gcc -O0 -g -Wall ground_truth_dump_enroll.c -o ground_truth_dump_enroll -ldl
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

#define OFF_FtGetTemplateForEnroll 0xb9e80
#define OFF_fp_device_verify      0x15a30
#define OFF_gFocalTempupdateInfor 0x30c5650

typedef struct { FP32 x, y, ori; UINT32 bDescri[8]; } ST_Feature;
typedef struct {
    ST_Feature *pTemplateFeature; UINT8 *templateBinDiscr; UINT8 *templatePixValid;
    UINT32 headerSize, featBufSize, binBufSize, maskBufSize, templateSize, templateBinDiscrLen;
    UINT16 templateExtendArea, subtemplatesPairIndex;
    FP32 subtemplatePairHmatrix[10];
    UINT8 templateCoinHmatrix[384];
    UINT8 nFeatureNum[2], templatePartsNum, templateArea, templateQuality, templateCondition,
          templateContrast, tempReplaceFlg, keepByte2, keepByte3, keepByte4, keepByte5, templateCoinFlag[25];
    UINT8 pad[7];
} ST_FocalTemplate;

typedef int (*FtGetTemplateForEnroll_fn)(UINT8 *image, ST_FocalTemplate *tpl);

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
    if (argc < 3) { fprintf(stderr, "usage: %s <so_path> <capture.raw>\n", argv[0]); return 1; }
    const char *soPath = argv[1], *rawPath = argv[2];
    const int rows = 80, cols = 64;

    void *handle = dlopen(soPath, RTLD_LAZY);
    if (!handle) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    void *anchor = dlsym(handle, "fp_device_verify");
    uintptr_t bias = (uintptr_t)anchor - OFF_fp_device_verify;
    FtGetTemplateForEnroll_fn fn = (FtGetTemplateForEnroll_fn)(bias + OFF_FtGetTemplateForEnroll);
    fprintf(stderr, "bias=0x%lx FtGetTemplateForEnroll @ %p\n", (unsigned long)bias, (void *)fn);

    void **gFocalTempupdateInfor_slot = (void **)(bias + OFF_gFocalTempupdateInfor);
    *gFocalTempupdateInfor_slot = calloc(1, 4096);

    unsigned char *img_tight = load_raw_be16_as_u8(rawPath, rows * cols);
    const int canvasDim = 96;
    unsigned char *img = calloc(1, (size_t)canvasDim * canvasDim + 32768);
    int padRows = (canvasDim - rows) / 2, padCols = (canvasDim - cols) / 2, rr;
    for (rr = 0; rr < rows; rr++)
        memcpy(img + (rr + padRows) * canvasDim + padCols, img_tight + rr * cols, (size_t)cols);
    free(img_tight);

    ST_FocalTemplate tpl;
    memset(&tpl, 0, sizeof(tpl));

    int ret = fn(img, &tpl);
    printf("FtGetTemplateForEnroll returned %d\n", ret);
    printf("nFeatureNum[0]=%d nFeatureNum[1]=%d templateArea=%d templateQuality=%d templateCondition=%d templateContrast=%d\n",
           tpl.nFeatureNum[0], tpl.nFeatureNum[1], tpl.templateArea, tpl.templateQuality, tpl.templateCondition, tpl.templateContrast);
    printf("pTemplateFeature=%p\n", (void *)tpl.pTemplateFeature);
    free(img);
    return 0;
}
