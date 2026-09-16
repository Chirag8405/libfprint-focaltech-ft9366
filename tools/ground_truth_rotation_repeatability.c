/*
 * DIAGNOSTIC-ONLY tool. Extends ground_truth_dump2.c's technique (direct
 * FtGetMfbFeatures call via dlopen+bias, bypassing FtGetTemplate's device-
 * init dependencies) to test the REAL algorithm's own detection
 * repeatability under a KNOWN synthetic rotation -- the same test
 * `test_synthetic_repeatability.c` runs against THIS PROJECT's
 * reimplementation. Purpose: determine whether this reimplementation's
 * detector is genuinely LESS rotation-robust than the real FocalTech
 * algorithm (a fixable bug) or whether the real algorithm shows similar
 * degradation (an inherent property of DoG-based detection at this
 * resolution, not a reimplementation bug). See research/PROTOCOL.md.
 *
 * NEVER shipped, NEVER used at runtime -- diagnostic-only, per this
 * project's standing rule on real .so calls.
 *
 * Build: gcc -O0 -g -Wall ground_truth_rotation_repeatability.c -o ground_truth_rotation_repeatability -ldl -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
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

/* Same rigid-transform convention as test_rotation_sweep.c/test_synthetic.c,
 * applied to the TIGHT 64x80 image before padding into the 96x96 canvas --
 * i.e. the exact same stage this project's own reimplementation applies its
 * synthetic transforms at, for a true apples-to-apples comparison. */
static void transform_image(const unsigned char *src, int rows, int cols,
                             double dx, double dy, double angleDeg, unsigned char *out)
{
    double rad = angleDeg * M_PI / 180.0;
    double cosA = cos(rad), sinA = sin(rad);
    double cx = cols / 2.0, cy = rows / 2.0;
    int r, c;
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            double ox = (c + 0.5) - cx - dx;
            double oy = (r + 0.5) - cy - dy;
            double sx = ox * cosA + oy * sinA + cx - 0.5;
            double sy = -ox * sinA + oy * cosA + cy - 0.5;
            int x0 = (int)floor(sx), y0 = (int)floor(sy);
            double tx = sx - x0, ty = sy - y0, v = 0.0;
            if (x0 >= 0 && x0 + 1 < cols && y0 >= 0 && y0 + 1 < rows) {
                double v00 = src[y0 * cols + x0], v01 = src[y0 * cols + x0 + 1];
                double v10 = src[(y0 + 1) * cols + x0], v11 = src[(y0 + 1) * cols + x0 + 1];
                double top = v00 + tx * (v01 - v00);
                double bot = v10 + tx * (v11 - v10);
                v = top + ty * (bot - top);
            }
            out[r * cols + c] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

static unsigned char *pad_to_canvas(const unsigned char *tight, int rows, int cols, int canvas)
{
    unsigned char *canvasBuf = calloc(1, (size_t)canvas * canvas);
    int padRows = (canvas - rows) / 2, padCols = (canvas - cols) / 2, r;
    for (r = 0; r < rows; r++)
        memcpy(canvasBuf + (r + padRows) * canvas + padCols, tight + r * cols, (size_t)cols);
    return canvasBuf;
}

static ST_EXTREMUM_NUM call_real_detector(FtGetMfbFeatures_fn fn, unsigned char *canvasBuf, int canvas,
                                           ST_Feature **feat1, ST_Feature **feat2)
{
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
    ST_EXTREMUM_NUM ret = fn(inPara, feat1, feat2);
    free(validMask); free(badPixMask);
    return ret;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <libfprint-2.so path> <capture.raw> [thresh_px]\n", argv[0]);
        return 1;
    }
    const char *soPath = argv[1], *rawPath = argv[2];
    double thresh = argc > 3 ? atof(argv[3]) : 3.0;
    const int rows = 80, cols = 64, canvas = 96;

    void *handle = dlopen(soPath, RTLD_LAZY);
    if (!handle) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    void *anchor = dlsym(handle, "fp_device_verify");
    if (!anchor) { fprintf(stderr, "dlsym failed: %s\n", dlerror()); return 1; }
    uintptr_t bias = (uintptr_t)anchor - OFF_fp_device_verify;
    FtGetMfbFeatures_fn FtGetMfbFeatures = (FtGetMfbFeatures_fn)(bias + OFF_FtGetMfbFeatures);

    unsigned char *base = load_raw_be16_as_u8(rawPath, rows * cols);

    /* Baseline (identity) call, for reference feature count. */
    unsigned char *canvas0 = pad_to_canvas(base, rows, cols, canvas);
    ST_Feature *feat1_0 = NULL, *feat2_0 = NULL;
    ST_EXTREMUM_NUM ret0 = call_real_detector(FtGetMfbFeatures, canvas0, canvas, &feat1_0, &feat2_0);
    int n0 = ret0.nMaxExtremum + ret0.nMinExtremum;
    printf("identity: nMax=%d nMin=%d total=%d\n", ret0.nMaxExtremum, ret0.nMinExtremum, n0);

    double transforms[][3] = { {0,0,0}, {2,0,0}, {0,2,0}, {0,0,2}, {0,0,3}, {0,0,5}, {2,1,3}, {-1,2,-2} };
    int nt = 8;
    double cx = cols / 2.0, cy = rows / 2.0;

    for (int t = 0; t < nt; t++) {
        double dx = transforms[t][0], dy = transforms[t][1], deg = transforms[t][2];
        unsigned char *variant = malloc((size_t)rows * cols);
        transform_image(base, rows, cols, dx, dy, deg, variant);
        unsigned char *canvasV = pad_to_canvas(variant, rows, cols, canvas);

        ST_Feature *feat1_v = NULL, *feat2_v = NULL;
        ST_EXTREMUM_NUM retV = call_real_detector(FtGetMfbFeatures, canvasV, canvas, &feat1_v, &feat2_v);
        int nv = retV.nMaxExtremum + retV.nMinExtremum;

        /* Combine feat1/feat2 into flat x,y arrays for both identity and variant. */
        int na = n0, nb = nv;
        double *ax = malloc(sizeof(double) * (na > 0 ? na : 1));
        double *ay = malloc(sizeof(double) * (na > 0 ? na : 1));
        double *bx = malloc(sizeof(double) * (nb > 0 ? nb : 1));
        double *by = malloc(sizeof(double) * (nb > 0 ? nb : 1));
        int k = 0;
        for (int i = 0; i < ret0.nMaxExtremum; i++) { ax[k] = feat1_0[i].x; ay[k] = feat1_0[i].y; k++; }
        for (int i = 0; i < ret0.nMinExtremum; i++) { ax[k] = feat2_0[i].x; ay[k] = feat2_0[i].y; k++; }
        k = 0;
        for (int i = 0; i < retV.nMaxExtremum; i++) { bx[k] = feat1_v[i].x; by[k] = feat1_v[i].y; k++; }
        for (int i = 0; i < retV.nMinExtremum; i++) { bx[k] = feat2_v[i].x; by[k] = feat2_v[i].y; k++; }

        /* KNOWN exact transform (same convention as test_synthetic_repeatability.c):
         * predict where identity's keypoint should land in the rotated canvas's
         * coordinate space, using the SAME center used for the image transform
         * (but in CANVAS coordinates, since real x,y are canvas-space -- offset
         * by the padding amount). */
        double padCols = (canvas - cols) / 2.0, padRows = (canvas - rows) / 2.0;
        double ccx = padCols + cx, ccy = padRows + cy;
        double rad = deg * M_PI / 180.0, c = cos(rad), s = sin(rad);
        int matched = 0;
        for (int i = 0; i < na; i++) {
            double px = ax[i] - ccx, py = ay[i] - ccy;
            double predx = px * c - py * s + ccx + dx;
            double predy = px * s + py * c + ccy + dy;
            double best = 1e18;
            for (int j = 0; j < nb; j++) {
                double ddx = predx - bx[j], ddy = predy - by[j];
                double d2 = ddx*ddx + ddy*ddy;
                if (d2 < best) best = d2;
            }
            if (na > 0 && best <= thresh*thresh) matched++;
        }
        printf("REAL ALGORITHM dx=%.0f dy=%.0f theta=%.0fdeg: na=%d nb=%d matched=%d rate=%.1f%% (thresh=%.1fpx)\n",
               dx, dy, deg, na, nb, matched, na>0 ? 100.0*matched/na : 0.0, thresh);

        free(ax); free(ay); free(bx); free(by);
        free(variant); free(canvasV);
    }
    return 0;
}
