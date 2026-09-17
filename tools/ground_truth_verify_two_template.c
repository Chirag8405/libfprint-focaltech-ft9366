/*
 * DIAGNOSTIC-ONLY tool. Calls the REAL FtGetTemplateForEnroll to build TWO
 * real ST_FocalTemplate structs from two real captures, then calls the REAL
 * FtVerifyTwoTemplate(tpl1, tpl2, &score, mode1, mode2) directly. Used to
 * live-verify the exact real candidate-generation/ratio-test logic inside
 * FtVerifyTwoTemplate (see research/PROTOCOL.md "alignment/correspondence
 * root-cause investigation") via gdb breakpoints at known offsets, rather
 * than relying purely on static disassembly reading for the accept/reject
 * branch sign, which is easy to get backwards by hand.
 *
 * NEVER shipped, NEVER used at runtime -- diagnostic-only.
 *
 * Build: gcc -O0 -g -Wall ground_truth_verify_two_template.c -o ground_truth_verify_two_template -ldl
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
#define OFF_FtVerifyTwoTemplate    0xc3770
#define OFF_fp_device_verify       0x15a30
#define OFF_gFocalTempupdateInfor  0x30c5650
#define OFF_gSensorInfor           0x211a80
#define OFF_FtSetSensorColRow      0xb8c30

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
typedef unsigned short (*FtVerifyTwoTemplate_fn)(ST_FocalTemplate *, ST_FocalTemplate *, FP32 *, UINT8, UINT8);
typedef void (*FtSetSensorColRow_fn)(int16_t, int16_t);

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

static unsigned char *build_canvas(const char *path)
{
    const int rows = 80, cols = 64, canvasDim = 96;
    unsigned char *tight = load_raw_be16_as_u8(path, rows * cols);
    unsigned char *img = calloc(1, (size_t)canvasDim * canvasDim + 32768);
    int padRows = (canvasDim - rows) / 2, padCols = (canvasDim - cols) / 2, rr;
    for (rr = 0; rr < rows; rr++)
        memcpy(img + (rr + padRows) * canvasDim + padCols, tight + rr * cols, (size_t)cols);
    free(tight);
    return img;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <so_path> <capture1.raw> <capture2.raw>\n", argv[0]); return 1; }
    const char *soPath = argv[1];

    void *handle = dlopen(soPath, RTLD_LAZY);
    if (!handle) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    void *anchor = dlsym(handle, "fp_device_verify");
    uintptr_t bias = (uintptr_t)anchor - OFF_fp_device_verify;

    void **gFocalTempupdateInfor_slot = (void **)(bias + OFF_gFocalTempupdateInfor);
    *gFocalTempupdateInfor_slot = calloc(1, 4096);

    FtSetSensorColRow_fn setColRow = (FtSetSensorColRow_fn)(bias + OFF_FtSetSensorColRow);
    setColRow(64, 80);

    FtGetTemplateForEnroll_fn getTpl = (FtGetTemplateForEnroll_fn)(bias + OFF_FtGetTemplateForEnroll);
    FtVerifyTwoTemplate_fn verify = (FtVerifyTwoTemplate_fn)(bias + OFF_FtVerifyTwoTemplate);

    unsigned char *img1 = build_canvas(argv[2]);
    unsigned char *img2 = build_canvas(argv[3]);

    ST_FocalTemplate tpl1, tpl2;
    memset(&tpl1, 0, sizeof(tpl1));
    memset(&tpl2, 0, sizeof(tpl2));

    int ret1 = getTpl(img1, &tpl1);
    int ret2 = getTpl(img2, &tpl2);
    fprintf(stderr, "enroll ret1=%d ret2=%d nFeat1=%d/%d nFeat2=%d/%d\n",
            ret1, ret2, tpl1.nFeatureNum[0], tpl1.nFeatureNum[1], tpl2.nFeatureNum[0], tpl2.nFeatureNum[1]);

    FP32 score = -1.0f;
    fprintf(stderr, "calling FtVerifyTwoTemplate at %p ...\n", (void *)verify);
    unsigned short ret = verify(&tpl1, &tpl2, &score, 0, 0);
    printf("FtVerifyTwoTemplate returned %u, score=%f\n", ret, score);

    return 0;
}
