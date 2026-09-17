/*
 * DIAGNOSTIC-ONLY tool. Step 2 of the orientation/mirroring hypothesis
 * check (research/PROTOCOL.md): for each REAL keypoint (x,y) the real
 * FtGetMfbFeatures reports (on this project's own captured+SPA-smoothed
 * 96x96 canvas), dump the actual local pixel neighborhood in THIS
 * project's own canvas buffer at that (x,y) and compute its local
 * variance. If this project's row-major (x=col,y=row) convention matches
 * what the real algorithm assumes, keypoints (which by construction are
 * DoG extrema -- local intensity structure) should sit on high-local-
 * variance (edge/ridge) content, not flat/uniform patches. Also computes
 * the average local variance at 20 random non-keypoint locations for
 * comparison.
 *
 * NEVER shipped, NEVER used at runtime -- diagnostic-only.
 *
 * Build: gcc -O0 -g -Wall ground_truth_coord_sanity.c -o ground_truth_coord_sanity -ldl -lm
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
static unsigned char *pad_to_canvas(const unsigned char *tight, int rows, int cols, int canvas)
{
    unsigned char *canvasBuf = calloc(1, (size_t)canvas * canvas);
    int padRows = (canvas - rows) / 2, padCols = (canvas - cols) / 2, r;
    for (r = 0; r < rows; r++)
        memcpy(canvasBuf + (r + padRows) * canvas + padCols, tight + r * cols, (size_t)cols);
    return canvasBuf;
}

/* local variance in a (2*rad+1)^2 window, treating canvasBuf as row-major [y*canvas+x] */
static double local_variance(const unsigned char *canvasBuf, int canvas, int x, int y, int rad)
{
    double sum = 0, sumsq = 0; int n = 0, dx, dy;
    for (dy = -rad; dy <= rad; dy++) {
        int yy = y + dy;
        if (yy < 0 || yy >= canvas) continue;
        for (dx = -rad; dx <= rad; dx++) {
            int xx = x + dx;
            if (xx < 0 || xx >= canvas) continue;
            double v = canvasBuf[yy * canvas + xx];
            sum += v; sumsq += v * v; n++;
        }
    }
    if (n == 0) return 0;
    double mean = sum / n;
    return sumsq / n - mean * mean;
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

    unsigned char *tight = load_raw_be16_as_u8(rawPath, rows * cols);
    unsigned char *canvasBuf = pad_to_canvas(tight, rows, cols, canvas);
    free(tight);

    spaInitSize((UINT16)canvas, (UINT16)canvas);
    spaInitRadius(5);
    spaInitImpact(0.0f);
    spaSmooth(canvasBuf, 20);

    unsigned char *mask = malloc((size_t)canvas * canvas);
    segFn(canvasBuf, canvas, canvas, 9, 12.0f, mask);

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
    printf("keypoints: max=%d min=%d\n", ret2.nMaxExtremum, ret2.nMinExtremum);

    printf("\n-- local variance (5x5 window) at first 10 real max-extrema keypoints --\n");
    int k;
    double sumKpVar = 0; int nKp = 0;
    for (k = 0; k < ret2.nMaxExtremum && k < 10; k++) {
        int x = (int)lroundf(feat1[k].x), y = (int)lroundf(feat1[k].y);
        double var = local_variance(canvasBuf, canvas, x, y, 2);
        printf("  kp[%d] x=%.2f y=%.2f -> localVar=%.1f centerPixel=%d\n",
               k, feat1[k].x, feat1[k].y, var, canvasBuf[y * canvas + x]);
        sumKpVar += var; nKp++;
    }
    for (k = 0; k < ret2.nMinExtremum && nKp < 20; k++) {
        int x = (int)lroundf(feat2[k].x), y = (int)lroundf(feat2[k].y);
        double var = local_variance(canvasBuf, canvas, x, y, 2);
        printf("  kp(min)[%d] x=%.2f y=%.2f -> localVar=%.1f centerPixel=%d\n",
               k, feat2[k].x, feat2[k].y, var, canvasBuf[y * canvas + x]);
        sumKpVar += var; nKp++;
    }

    srand(12345);
    double sumRandVar = 0; int nRand = 0, r;
    printf("\n-- local variance (5x5 window) at 20 random non-keypoint locations --\n");
    for (r = 0; r < 20; r++) {
        int x = 8 + rand() % (canvas - 16), y = 8 + rand() % (canvas - 16);
        double var = local_variance(canvasBuf, canvas, x, y, 2);
        printf("  rand[%d] x=%d y=%d -> localVar=%.1f centerPixel=%d\n", r, x, y, var, canvasBuf[y * canvas + x]);
        sumRandVar += var; nRand++;
    }

    printf("\n=== SUMMARY: avg localVar at real keypoints = %.1f (n=%d), avg localVar at random locations = %.1f (n=%d) ===\n",
           sumKpVar / nKp, nKp, sumRandVar / nRand, nRand);

    /* Also dump a coarse ASCII map of the canvas + mask, and mark keypoint
     * locations, so the whole picture (finger silhouette shape) can be
     * sanity-checked by eye against where keypoints and mask agree. */
    printf("\n-- coarse ASCII map (mask=# unmasked=., real keypoints=X), 96x96 downsampled to 48x48 --\n");
    int yy, xx;
    for (yy = 0; yy < canvas; yy += 2) {
        for (xx = 0; xx < canvas; xx += 2) {
            int isKp = 0;
            for (k = 0; k < ret2.nMaxExtremum; k++) {
                int kx = (int)lroundf(feat1[k].x), ky = (int)lroundf(feat1[k].y);
                if (abs(kx - xx) <= 1 && abs(ky - yy) <= 1) { isKp = 1; break; }
            }
            if (isKp) putchar('X');
            else putchar(mask[yy * canvas + xx] ? '#' : '.');
        }
        putchar('\n');
    }

    return 0;
}
