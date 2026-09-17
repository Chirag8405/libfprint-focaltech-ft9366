/*
 * DIAGNOSTIC-ONLY tool. Tests the mirroring/orientation hypothesis (see
 * research/PROTOCOL.md "Windows-vs-Linux discrepancy investigation --
 * orientation/mirroring hypothesis"): feeds ONE real capture's raw buffer
 * into the real FtSegmentByLocalVariance + FtGetMfbFeatures pipeline
 * (with the real, confirmed SPA smoothing step from STEP 1) in FOUR
 * orientations -- original, horizontal flip, vertical flip, 180-degree
 * rotation -- and compares the REAL algorithm's own detected keypoint
 * count/positions/orientations across all four, to check whether one
 * orientation produces meaningfully different (better/worse) detection
 * structure than the others.
 *
 * NEVER shipped, NEVER used at runtime -- diagnostic-only.
 *
 * Build: gcc -O0 -g -Wall ground_truth_orientation_test.c -o ground_truth_orientation_test -ldl -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <dlfcn.h>

typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef int32_t  SINT32;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef float    FP32;

#define OFF_fp_device_verify         0x15a30
#define OFF_FtGetMfbFeatures         0xdab10
#define OFF_FtSegmentByLocalVariance 0xe3600
#define OFF_gFocalTempupdateInfor    0x30c5650
#define OFF_InitSPAImageSize         0x11aba0
#define OFF_InitSPAMaskRadius        0x11abb0
#define OFF_InitSPAImpactFactors     0x11abc0
#define OFF_FtSpaSmooth              0x11abd0

typedef struct {
    SINT32 depth, width, height, imageSize, widthStep;
    UINT8 *imageData;
} ST_IplImage;

typedef struct {
    SINT32 intvls; FP32 sigma; FP32 contrThr; SINT32 curvThr; SINT32 imgDbl;
    SINT32 descrWidth; SINT32 descrHistBins; ST_IplImage *img;
    UINT8 octave; UINT8 validArea; UINT8 *validFlg; UINT8 *badPixselValidFlg;
    UINT8 isFT9391; UINT8 algType; UINT8 sensorCol; UINT8 isSpeedUp; FP32 imgScale;
} ST_InputForTemplate;

typedef struct { FP32 x, y, ori; UINT32 bDescri[8]; } ST_Feature;
typedef struct { SINT32 nMaxExtremum, nMinExtremum; } ST_EXTREMUM_NUM;

typedef ST_EXTREMUM_NUM (*FtGetMfbFeatures_fn)(ST_InputForTemplate, ST_Feature **, ST_Feature **);
typedef int (*FtSegmentByLocalVariance_fn)(UINT8 *, SINT32, SINT32, SINT32, FP32, UINT8 *);
typedef void (*InitSPAImageSize_fn)(UINT16, UINT16);
typedef void (*InitSPAMaskRadius_fn)(UINT16);
typedef void (*InitSPAImpactFactors_fn)(FP32);
typedef unsigned char (*FtSpaSmooth_fn)(UINT8 *, UINT16);

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

/* orientation: 0=original, 1=hflip, 2=vflip, 3=180rot */
static unsigned char *transform_tight(const unsigned char *tight, int rows, int cols, int orientation)
{
    unsigned char *out = malloc((size_t)rows * cols);
    int r, c;
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            int sr = r, sc = c;
            if (orientation == 1) sc = cols - 1 - c;
            else if (orientation == 2) sr = rows - 1 - r;
            else if (orientation == 3) { sr = rows - 1 - r; sc = cols - 1 - c; }
            out[r * cols + c] = tight[sr * cols + sc];
        }
    }
    return out;
}

static unsigned char *pad_to_canvas(const unsigned char *tight, int rows, int cols, int canvas)
{
    unsigned char *canvasBuf = calloc(1, (size_t)canvas * canvas);
    int padRows = (canvas - rows) / 2, padCols = (canvas - cols) / 2, r;
    for (r = 0; r < rows; r++)
        memcpy(canvasBuf + (r + padRows) * canvas + padCols, tight + r * cols, (size_t)cols);
    return canvasBuf;
}

static const char *orient_name(int o)
{
    switch (o) {
        case 0: return "original";
        case 1: return "hflip";
        case 2: return "vflip";
        case 3: return "rot180";
    }
    return "?";
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <so_path> <capture.raw>\n", argv[0]); return 1; }
    const char *soPath = argv[1], *rawPath = argv[2];
    const int rows = 80, cols = 64, canvas = 96;

    void *handle = dlopen(soPath, RTLD_LAZY);
    if (!handle) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    void *anchor = dlsym(handle, "fp_device_verify");
    uintptr_t bias = (uintptr_t)anchor - OFF_fp_device_verify;

    void **gFocalTempupdateInfor_slot = (void **)(bias + OFF_gFocalTempupdateInfor);
    *gFocalTempupdateInfor_slot = calloc(1, 4096);

    FtSegmentByLocalVariance_fn segFn = (FtSegmentByLocalVariance_fn)(bias + OFF_FtSegmentByLocalVariance);
    FtGetMfbFeatures_fn featFn = (FtGetMfbFeatures_fn)(bias + OFF_FtGetMfbFeatures);
    InitSPAImageSize_fn spaInitSize = (InitSPAImageSize_fn)(bias + OFF_InitSPAImageSize);
    InitSPAMaskRadius_fn spaInitRadius = (InitSPAMaskRadius_fn)(bias + OFF_InitSPAMaskRadius);
    InitSPAImpactFactors_fn spaInitImpact = (InitSPAImpactFactors_fn)(bias + OFF_InitSPAImpactFactors);
    FtSpaSmooth_fn spaSmooth = (FtSpaSmooth_fn)(bias + OFF_FtSpaSmooth);

    unsigned char *tight0 = load_raw_be16_as_u8(rawPath, rows * cols);

    int o;
    for (o = 0; o < 4; o++) {
        unsigned char *tight = transform_tight(tight0, rows, cols, o);
        unsigned char *canvasBuf = pad_to_canvas(tight, rows, cols, canvas);
        free(tight);

        spaInitSize((UINT16)canvas, (UINT16)canvas);
        spaInitRadius(5);
        spaInitImpact(0.0f);
        spaSmooth(canvasBuf, 20);

        unsigned char *mask = malloc((size_t)canvas * canvas);
        segFn(canvasBuf, canvas, canvas, 9, 12.0f, mask);
        int maskCount = 0, i;
        for (i = 0; i < canvas * canvas; i++) if (mask[i]) maskCount++;

        ST_IplImage iplImage = {
            .depth = 8, .width = canvas, .height = canvas,
            .imageSize = canvas * canvas, .widthStep = canvas, .imageData = canvasBuf
        };
        unsigned char *validMask = malloc(1152); memset(validMask, 0xFF, 1152);
        unsigned char *badPixMask = malloc(1152); memset(badPixMask, 0xFF, 1152);
        ST_InputForTemplate inPara = {
            .intvls = 3, .sigma = 1.6f, .contrThr = 0.02f, .curvThr = 15,
            .imgDbl = 1, .descrWidth = 4, .descrHistBins = 8, .img = &iplImage,
            .octave = 4, .validArea = 100, .validFlg = validMask, .badPixselValidFlg = badPixMask,
            .isFT9391 = 0, .algType = 0, .sensorCol = 96, .isSpeedUp = 0, .imgScale = 1.5f
        };
        ST_Feature *feat1 = NULL, *feat2 = NULL;
        ST_EXTREMUM_NUM ret2 = featFn(inPara, &feat1, &feat2);
        int n = ret2.nMaxExtremum + ret2.nMinExtremum;

        printf("=== %s: maskArea=%d keypoints=%d (max=%d min=%d) ===\n",
               orient_name(o), maskCount, n, ret2.nMaxExtremum, ret2.nMinExtremum);
        int k, shown = 0;
        for (k = 0; k < ret2.nMaxExtremum && shown < 5; k++, shown++)
            printf("  max[%d]: x=%.2f y=%.2f ori=%.4f\n", k, feat1[k].x, feat1[k].y, feat1[k].ori);
        shown = 0;
        for (k = 0; k < ret2.nMinExtremum && shown < 5; k++, shown++)
            printf("  min[%d]: x=%.2f y=%.2f ori=%.4f\n", k, feat2[k].x, feat2[k].y, feat2[k].ori);

        free(validMask); free(badPixMask); free(mask); free(canvasBuf);
    }
    free(tight0);
    return 0;
}
