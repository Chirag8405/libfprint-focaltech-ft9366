/*
 * DIAGNOSTIC-ONLY tool, v2. Calls FtGetMfbFeatures DIRECTLY (not
 * FtGetTemplate) with a manually-constructed, fully-controlled
 * ST_IplImage + ST_InputForTemplate, to bypass FtGetTemplate's own deep
 * chain of session/device-init state dependencies (FtGetFlag, pData1/
 * pData2, etc. -- see research/PROTOCOL.md) that made completing a full
 * FtGetTemplate call impractical.
 *
 * NEVER shipped, NEVER used at runtime -- diagnostic-only, per this
 * session's explicit decision (research/PROTOCOL.md).
 *
 * Build: gcc -O0 -g -Wall ground_truth_dump2.c -o ground_truth_dump2 -ldl
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

typedef uint8_t  UINT8;
typedef int32_t  SINT32;
typedef uint32_t UINT32;
typedef float    FP32;

#define OFF_FtGetMfbFeatures  0xdab10
#define OFF_fp_device_verify  0x15a30

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
    const int rows = 80, cols = 64, canvas = 96;

    void *handle = dlopen(soPath, RTLD_LAZY);
    if (!handle) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    void *anchor = dlsym(handle, "fp_device_verify");
    if (!anchor) { fprintf(stderr, "dlsym failed: %s\n", dlerror()); return 1; }
    uintptr_t bias = (uintptr_t)anchor - OFF_fp_device_verify;
    FtGetMfbFeatures_fn FtGetMfbFeatures = (FtGetMfbFeatures_fn)(bias + OFF_FtGetMfbFeatures);
    fprintf(stderr, "bias=0x%lx FtGetMfbFeatures @ %p\n", (unsigned long)bias, (void *)FtGetMfbFeatures);

    unsigned char *img_tight = load_raw_be16_as_u8(rawPath, rows * cols);
    unsigned char *canvasBuf = calloc(1, (size_t)canvas * canvas);
    int padRows = (canvas - rows) / 2, padCols = (canvas - cols) / 2, r;
    for (r = 0; r < rows; r++)
        memcpy(canvasBuf + (r + padRows) * canvas + padCols, img_tight + r * cols, (size_t)cols);
    free(img_tight);

    ST_IplImage iplImage = {
        .depth = 8, .width = canvas, .height = canvas,
        .imageSize = canvas * canvas, .widthStep = canvas, .imageData = canvasBuf
    };

    unsigned char *validMask = malloc(1152); memset(validMask, 0xFF, 1152); /* ceil(9216/8) */
    unsigned char *badPixMask = malloc(1152); memset(badPixMask, 0xFF, 1152);

    ST_InputForTemplate inPara = {
        .intvls = 3, .sigma = 1.6f, .contrThr = 0.02f, .curvThr = 15,
        .imgDbl = 1, .descrWidth = 4, .descrHistBins = 8, .img = &iplImage,
        .octave = 4, .validArea = 100, .validFlg = validMask, .badPixselValidFlg = badPixMask,
        .isFT9391 = 0, .algType = 0, .sensorCol = 96, .isSpeedUp = 0, .imgScale = 1.5f
    };

    ST_Feature *feat1 = NULL, *feat2 = NULL;
    ST_EXTREMUM_NUM ret = FtGetMfbFeatures(inPara, &feat1, &feat2);

    FILE *out = fopen(outPath, "w");
    fprintf(out, "# DIAGNOSTIC-ONLY ground truth dump (v2, direct FtGetMfbFeatures call)\n");
    fprintf(out, "# source_image=%s\n", rawPath);
    fprintf(out, "# nMaxExtremum=%d nMinExtremum=%d\n", ret.nMaxExtremum, ret.nMinExtremum);
    fprintf(out, "# feat1=%p feat2=%p\n", (void *)feat1, (void *)feat2);

    int i;
    if (feat1 && ret.nMaxExtremum > 0) {
        fprintf(out, "# feat1 (max extrema), %d entries\n", ret.nMaxExtremum);
        for (i = 0; i < ret.nMaxExtremum && i < 500; i++) {
            ST_Feature *f = &feat1[i];
            fprintf(out, "1 %d %.6f %.6f %.6f", i, f->x, f->y, f->ori);
            int k; for (k = 0; k < 8; k++) fprintf(out, " %08x", f->bDescri[k]);
            fprintf(out, "\n");
        }
    }
    if (feat2 && ret.nMinExtremum > 0) {
        fprintf(out, "# feat2 (min extrema), %d entries\n", ret.nMinExtremum);
        for (i = 0; i < ret.nMinExtremum && i < 500; i++) {
            ST_Feature *f = &feat2[i];
            fprintf(out, "2 %d %.6f %.6f %.6f", i, f->x, f->y, f->ori);
            int k; for (k = 0; k < 8; k++) fprintf(out, " %08x", f->bDescri[k]);
            fprintf(out, "\n");
        }
    }
    fclose(out);
    printf("wrote %s (nMax=%d nMin=%d)\n", outPath, ret.nMaxExtremum, ret.nMinExtremum);
    return 0;
}
