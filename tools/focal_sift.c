/*
 * Clean-room reimplementation of the FocalTech "MFS/MFB" keypoint
 * detection + steered binary descriptor pipeline (Steps 2-3), built from
 * this project's own reverse-engineering (research/PROTOCOL.md) --
 * zero runtime dependency on the proprietary .so.
 *
 * Status: FtGetMfsFeatures/FtGetMfbFeatures were confirmed (call-graph +
 * struct-field-name correspondence, cross-checked against the actual
 * public OpenSIFT source at github.com/robwhess/opensift) to be an
 * adapted port of OpenSIFT for detection/orientation, with a bespoke
 * steered concentric-ring binary descriptor (FtMfbDescriptors) in place
 * of OpenSIFT's own float descriptor. The pyramid/extrema/orientation
 * code below is a direct translation of OpenSIFT's public algorithm
 * (CONFIRMED structural match), using this project's own two confirmed
 * real parameter deviations (contrThr=0.02, curvThr=15). The descriptor
 * code uses the exact data tables extracted from .rodata
 * (focal_tables.h). Everything here is a first-draft, UNTESTED
 * reimplementation -- not yet validated against real captures.
 *
 * Build: gcc -O2 -Wall -c focal_sift.c -o focal_sift.o -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "focal_tables.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- confirmed real tuning constants (research/PROTOCOL.md,
 * "MAJOR FINDING: FtGetMfsFeatures is an adapted OpenSIFT implementation") ---- */
#define FOCAL_INTVLS    3
#define FOCAL_SIGMA     1.6
#define FOCAL_CONTR_THR 0.02   /* half of OpenSIFT's stock 0.04 */
#define FOCAL_CURV_THR  15     /* OpenSIFT's stock is 10 */
#define FOCAL_IMG_DBL   1      /* strong indirect evidence, see PROTOCOL.md */
/* CONFIRMED via real .so ground truth (research/PROTOCOL.md, "MAJOR
 * GROUND-TRUTH FINDING"): the real algorithm's working canvas is a FIXED
 * 96x96 (gSensorInfor.sensorCols/sensorRows), NOT this sensor's native
 * 64x80 wire resolution. (The doubled-scale factor is covered by the
 * comment on FOCAL_DBL_SCALE just below -- see that for the corrected
 * 2.0x finding.) */
#define FOCAL_WORKING_CANVAS 96
/* CORRECTED (this continuation session): direct inspection of the real
 * FtMfbDescriptors's actual gauss_pyr[0][0] via gdb shows a REAL 192x192
 * float image (depth=32) -- exactly 96*2, standard OpenSIFT-stock 2x
 * doubling, NOT 1.5x. The earlier "1.5x" finding (imgSizeScale=144x144
 * inside FtGetTemplate) was real but misapplied here -- it belongs to a
 * DIFFERENT FtGetTemplate-internal stage (not yet identified), not this
 * SIFT-like detection pyramid's img_dbl step. Also confirmed 4 real
 * octaves (192/96/48/24), not 3. */
#define FOCAL_DBL_SCALE 2.0

/* ---- OpenSIFT stock constants not confirmed to be overridden ---- */
#define SIFT_INIT_SIGMA        0.5
#define SIFT_IMG_BORDER        5
#define SIFT_MAX_INTERP_STEPS  5
#define SIFT_ORI_HIST_BINS     36
#define SIFT_ORI_SIG_FCTR      1.5
#define SIFT_ORI_RADIUS        (3.0 * SIFT_ORI_SIG_FCTR)
#define SIFT_ORI_SMOOTH_PASSES 2
#define SIFT_ORI_PEAK_RATIO    0.8

typedef struct { int rows, cols; float *data; } FImage;

static FImage *img_new(int rows, int cols)
{
    FImage *im = malloc(sizeof(FImage));
    im->rows = rows; im->cols = cols;
    im->data = calloc((size_t)rows * cols, sizeof(float));
    return im;
}
static void img_free(FImage *im) { if (im) { free(im->data); free(im); } }
static float img_get(const FImage *im, int r, int c)
{
    if (r < 0) r = 0;
    if (r >= im->rows) r = im->rows - 1;
    if (c < 0) c = 0;
    if (c >= im->cols) c = im->cols - 1;
    return im->data[r * im->cols + c];
}
static void img_set(FImage *im, int r, int c, float v) { im->data[r * im->cols + c] = v; }

/* CONFIRMED via raw disassembly of FastGaussBlur/AverageFilter (offsets
 * 0xf2940/0xed770, research/PROTOCOL.md "full raw disassembly of
 * FtBuildGaussPyr/FtCreateInitImg"): the real algorithm's pyramid blur is
 * NOT a true Gaussian kernel convolution at all -- it is a short cascade
 * of separable, UNWEIGHTED box-average filters (3x3, occasionally 5x5).
 * The exact iteration count and kernel sizes come from fixed lookup
 * tables (FILTER_ITER1/KSIZE_TAB1 -- confirmed selected by the real
 * runtime value of a global mode byte, gAlgMode[5]=4, verified live via
 * gdb, not just its static .data initializer), indexed DIRECTLY by
 * pyramid interval number (0-5 for intvls=3) -- NOT computed from any
 * continuous per-level sigma formula. This replaces this reimplementation's
 * previous `gaussian_blur_f` (a true, wide discrete Gaussian kernel),
 * which -- despite two separate correctness passes (bicubic-vs-bilinear
 * upscale, then kernel-truncation-radius) -- could never match the real
 * pyramid's content because it was fundamentally the wrong OPERATOR, not
 * an imprecise version of the right one. */
static void box_filter_1d_shrink(const float *src, float *dst, int n, int ksize)
{
    /* CONFIRMED border handling: a SHRINKING window at the edges,
     * normalized by the ACTUAL number of samples included (verified via
     * the real algorithm's own `invTable[n] = 1/n` reciprocal lookup
     * table, extracted from .rodata) -- NOT edge-replication/clamping. */
    int half = ksize / 2;
    int i, k;
    for (i = 0; i < n; i++) {
        int lo = i - half; if (lo < 0) lo = 0;
        int hi = i + half; if (hi >= n) hi = n - 1;
        float sum = 0;
        for (k = lo; k <= hi; k++) sum += src[k];
        dst[i] = sum / (float)(hi - lo + 1);
    }
}
static void average_filter_2d(FImage *im, int ksize)
{
    /* CONFIRMED structural match: one full separable 2D box-average pass
     * (horizontal then vertical, same ksize both directions) -- matches
     * one AverageFilter call in the real disassembly. */
    if (ksize <= 1) return;
    int rows = im->rows, cols = im->cols, r, c;
    float *rowbuf = malloc((size_t)cols * sizeof(float));
    float *rowout = malloc((size_t)cols * sizeof(float));
    for (r = 0; r < rows; r++) {
        memcpy(rowbuf, &im->data[r * cols], (size_t)cols * sizeof(float));
        box_filter_1d_shrink(rowbuf, rowout, cols, ksize);
        memcpy(&im->data[r * cols], rowout, (size_t)cols * sizeof(float));
    }
    free(rowbuf); free(rowout);
    float *colbuf = malloc((size_t)rows * sizeof(float));
    float *colout = malloc((size_t)rows * sizeof(float));
    for (c = 0; c < cols; c++) {
        for (r = 0; r < rows; r++) colbuf[r] = im->data[r * cols + c];
        box_filter_1d_shrink(colbuf, colout, rows, ksize);
        for (r = 0; r < rows; r++) im->data[r * cols + c] = colout[r];
    }
    free(colbuf); free(colout);
}
static void fast_gauss_blur(FImage *im, int tableIndex)
{
    /* CONFIRMED exact table contents, extracted from .rodata
     * (FILTER_ITER1 @0x1a9508, KSIZE_TAB1 @0x1a94c0). tableIndex is the
     * pyramid interval number (0-5), used directly as the row index. */
    static const int iterCounts[6] = {4, 3, 3, 3, 3, 3};
    static const int ksizeTab[6][6] = {
        {3, 3, 3, 3, 3, 3},
        {3, 3, 3, 0, 0, 0},
        {3, 3, 3, 0, 0, 0},
        {3, 3, 5, 0, 0, 0},
        {3, 3, 5, 0, 0, 0},
        {3, 3, 5, 0, 0, 0},
    };
    int iters = iterCounts[tableIndex];
    int k;
    for (k = 0; k < iters; k++) average_filter_2d(im, ksizeTab[tableIndex][k]);
}

/* OpenCV-style bicubic (a=-0.75, matching OpenCV's actual INTER_CUBIC
 * constant) -- tested hypothesis for the img_dbl upscale step, per
 * research/PROTOCOL.md ("Leading hypotheses for the remaining sub-pixel
 * discrepancy"): OpenSIFT's stock create_init_img uses CV_INTER_CUBIC for
 * doubling, not bilinear. */
static double cubic_weight(double x)
{
    const double a = -0.75;
    x = fabs(x);
    if (x <= 1.0) return (a + 2) * x * x * x - (a + 3) * x * x + 1;
    if (x < 2.0) return a * x * x * x - 5 * a * x * x + 8 * a * x - 4 * a;
    return 0.0;
}
static FImage *resize_bicubic(const FImage *src, int newRows, int newCols)
{
    FImage *dst = img_new(newRows, newCols);
    int r, c, i, j;
    double rowScale = (double)src->rows / newRows;
    double colScale = (double)src->cols / newCols;
    for (r = 0; r < newRows; r++) {
        double sr = (r + 0.5) * rowScale - 0.5;
        int r0 = (int)floor(sr);
        double fr = sr - r0;
        double wr[4]; for (i = -1; i <= 2; i++) wr[i + 1] = cubic_weight(i - fr);
        for (c = 0; c < newCols; c++) {
            double sc = (c + 0.5) * colScale - 0.5;
            int c0 = (int)floor(sc);
            double fc = sc - c0;
            double wc[4]; for (j = -1; j <= 2; j++) wc[j + 1] = cubic_weight(j - fc);
            double acc = 0;
            for (i = -1; i <= 2; i++)
                for (j = -1; j <= 2; j++)
                    acc += wr[i + 1] * wc[j + 1] * img_get(src, r0 + i, c0 + j);
            img_set(dst, r, c, (float)acc);
        }
    }
    return dst;
}

/* CONFIRMED structural match: OpenSIFT's create_init_img. */
static FImage *create_init_img(const unsigned char *src, int rows, int cols,
                                int img_dbl, double sigma)
{
    (void)sigma; /* CONFIRMED (this session): the real blur is a fixed
                  * box-filter cascade (fast_gauss_blur), not a computed-
                  * sigma Gaussian -- sigma is no longer used to derive the
                  * initial blur amount. Kept in the signature to avoid
                  * churning call sites. */
    FImage *gray = img_new(rows, cols);
    int i;
    for (i = 0; i < rows * cols; i++) gray->data[i] = (float)src[i];

    if (img_dbl) {
        /* CONFIRMED real scale factor is 2.0x (OpenSIFT stock), per this
         * continuation session's direct gauss_pyr inspection -- corrects
         * the previous session's mistaken 1.5x conclusion. Now using
         * bicubic for the upscale (kept -- FtDoubleImageByCubic's name
         * confirms cubic, though its exact coefficient was not
         * independently re-verified this pass) then the CONFIRMED
         * box-filter-cascade blur (fast_gauss_blur, tableIndex=0) instead
         * of a computed-sigma true Gaussian -- see fast_gauss_blur above. */
        double s = FOCAL_DBL_SCALE;
        FImage *dbl = resize_bicubic(gray, (int)lround(rows * s), (int)lround(cols * s));
        fast_gauss_blur(dbl, 0);
        img_free(gray);
        return dbl;
    } else {
        fast_gauss_blur(gray, 0);
        return gray;
    }
}

/* CONFIRMED structural match: OpenSIFT's downsample (CV_INTER_NN --
 * nearest neighbor, not bilinear; audit fix, see PROTOCOL.md). */
static FImage *downsample_nn(const FImage *src, int newRows, int newCols)
{
    FImage *dst = img_new(newRows, newCols);
    double rowScale = (double)src->rows / newRows;
    double colScale = (double)src->cols / newCols;
    int r, c;
    for (r = 0; r < newRows; r++)
        for (c = 0; c < newCols; c++) {
            int sr = (int)(r * rowScale);
            int sc = (int)(c * colScale);
            img_set(dst, r, c, img_get(src, sr, sc));
        }
    return dst;
}

/* CONFIRMED structural match: OpenSIFT's build_gauss_pyr, EXCEPT the blur
 * amount itself (see fast_gauss_blur above): raw disassembly of
 * FtBuildGaussPyr shows the per-interval blur call passes the INTERVAL
 * NUMBER ITSELF (0..intvls+2) directly as FastGaussBlur's table index --
 * there is no continuously-computed per-level sigma at all. `sigma` is
 * kept in the signature (unused now) to avoid churning call sites. */
static FImage ***build_gauss_pyr(FImage *base, int octvs, int intvls, double sigma)
{
    (void)sigma;
    FImage ***pyr = malloc((size_t)octvs * sizeof(FImage **));
    int i, o;

    for (o = 0; o < octvs; o++) {
        pyr[o] = malloc((size_t)(intvls + 3) * sizeof(FImage *));
        for (i = 0; i < intvls + 3; i++) {
            if (o == 0 && i == 0) {
                pyr[o][i] = img_new(base->rows, base->cols);
                memcpy(pyr[o][i]->data, base->data, (size_t)base->rows * base->cols * sizeof(float));
            } else if (i == 0) {
                FImage *prev = pyr[o - 1][intvls];
                pyr[o][i] = downsample_nn(prev, prev->rows / 2, prev->cols / 2);
            } else {
                pyr[o][i] = img_new(pyr[o][i - 1]->rows, pyr[o][i - 1]->cols);
                memcpy(pyr[o][i]->data, pyr[o][i - 1]->data,
                       (size_t)pyr[o][i - 1]->rows * pyr[o][i - 1]->cols * sizeof(float));
                fast_gauss_blur(pyr[o][i], i);
            }
        }
    }
    return pyr;
}

/* CONFIRMED structural match: OpenSIFT's build_dog_pyr. */
static FImage ***build_dog_pyr(FImage ***gauss_pyr, int octvs, int intvls)
{
    FImage ***pyr = malloc((size_t)octvs * sizeof(FImage **));
    int i, o;
    for (o = 0; o < octvs; o++) {
        pyr[o] = malloc((size_t)(intvls + 2) * sizeof(FImage *));
        for (i = 0; i < intvls + 2; i++) {
            FImage *a = gauss_pyr[o][i + 1], *b = gauss_pyr[o][i];
            pyr[o][i] = img_new(a->rows, a->cols);
            int n = a->rows * a->cols, j;
            for (j = 0; j < n; j++) pyr[o][i]->data[j] = a->data[j] - b->data[j];
        }
    }
    return pyr;
}

static void free_pyr(FImage ***pyr, int octvs, int levels)
{
    int o, i;
    for (o = 0; o < octvs; o++) {
        for (i = 0; i < levels; i++) img_free(pyr[o][i]);
        free(pyr[o]);
    }
    free(pyr);
}

typedef struct {
    float x, y, ori;
    int octv, intvl, r, c;
    float subintvl, scl, scl_octv;
    float local_x, local_y; /* position in gauss_pyr[octv][intvl]'s OWN
                                pixel grid -- unaffected by octave scaling
                                or img_dbl adjustment, unlike x/y. Used for
                                descriptor sampling. */
} FocalKeypoint;

typedef struct { FocalKeypoint *items; int n, cap; } KpList;
static void kp_push(KpList *l, FocalKeypoint kp)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->items = realloc(l->items, (size_t)l->cap * sizeof(FocalKeypoint));
    }
    l->items[l->n++] = kp;
}

/* CONFIRMED structural match: OpenSIFT's deriv_3D/hessian_3D (FtDeriv3D/
 * FtHessian3D), matched via disassembly to central-difference formulas. */
static void deriv_3D(FImage ***dog, int o, int i, int r, int c, double d[3])
{
    d[0] = (img_get(dog[o][i], r, c + 1) - img_get(dog[o][i], r, c - 1)) / 2.0;
    d[1] = (img_get(dog[o][i], r + 1, c) - img_get(dog[o][i], r - 1, c)) / 2.0;
    d[2] = (img_get(dog[o][i + 1], r, c) - img_get(dog[o][i - 1], r, c)) / 2.0;
}
static void hessian_3D(FImage ***dog, int o, int i, int r, int c, double H[3][3])
{
    double v = img_get(dog[o][i], r, c);
    double dxx = img_get(dog[o][i], r, c + 1) + img_get(dog[o][i], r, c - 1) - 2 * v;
    double dyy = img_get(dog[o][i], r + 1, c) + img_get(dog[o][i], r - 1, c) - 2 * v;
    double dss = img_get(dog[o][i + 1], r, c) + img_get(dog[o][i - 1], r, c) - 2 * v;
    double dxy = (img_get(dog[o][i], r + 1, c + 1) - img_get(dog[o][i], r + 1, c - 1)
                - img_get(dog[o][i], r - 1, c + 1) + img_get(dog[o][i], r - 1, c - 1)) / 4.0;
    double dxs = (img_get(dog[o][i + 1], r, c + 1) - img_get(dog[o][i + 1], r, c - 1)
                - img_get(dog[o][i - 1], r, c + 1) + img_get(dog[o][i - 1], r, c - 1)) / 4.0;
    double dys = (img_get(dog[o][i + 1], r + 1, c) - img_get(dog[o][i + 1], r - 1, c)
                - img_get(dog[o][i - 1], r + 1, c) + img_get(dog[o][i - 1], r - 1, c)) / 4.0;
    H[0][0] = dxx; H[0][1] = dxy; H[0][2] = dxs;
    H[1][0] = dxy; H[1][1] = dyy; H[1][2] = dys;
    H[2][0] = dxs; H[2][1] = dys; H[2][2] = dss;
}
static int invert3x3(double H[3][3], double inv[3][3])
{
    double det = H[0][0] * (H[1][1] * H[2][2] - H[1][2] * H[2][1])
               - H[0][1] * (H[1][0] * H[2][2] - H[1][2] * H[2][0])
               + H[0][2] * (H[1][0] * H[2][1] - H[1][1] * H[2][0]);
    if (fabs(det) < 1e-12) return 0;
    double id = 1.0 / det;
    inv[0][0] = (H[1][1] * H[2][2] - H[1][2] * H[2][1]) * id;
    inv[0][1] = (H[0][2] * H[2][1] - H[0][1] * H[2][2]) * id;
    inv[0][2] = (H[0][1] * H[1][2] - H[0][2] * H[1][1]) * id;
    inv[1][0] = (H[1][2] * H[2][0] - H[1][0] * H[2][2]) * id;
    inv[1][1] = (H[0][0] * H[2][2] - H[0][2] * H[2][0]) * id;
    inv[1][2] = (H[0][2] * H[1][0] - H[0][0] * H[1][2]) * id;
    inv[2][0] = (H[1][0] * H[2][1] - H[1][1] * H[2][0]) * id;
    inv[2][1] = (H[0][1] * H[2][0] - H[0][0] * H[2][1]) * id;
    inv[2][2] = (H[0][0] * H[1][1] - H[0][1] * H[1][0]) * id;
    return 1;
}
/* CONFIRMED structural match: OpenSIFT's interp_step (FtInterpStep). */
static int interp_step(FImage ***dog, int o, int i, int r, int c,
                        double *xi, double *xr, double *xc)
{
    double dD[3], H[3][3], Hinv[3][3];
    deriv_3D(dog, o, i, r, c, dD);
    hessian_3D(dog, o, i, r, c, H);
    if (!invert3x3(H, Hinv)) { *xi = *xr = *xc = 0; return 0; }
    *xc = -(Hinv[0][0] * dD[0] + Hinv[0][1] * dD[1] + Hinv[0][2] * dD[2]);
    *xr = -(Hinv[1][0] * dD[0] + Hinv[1][1] * dD[1] + Hinv[1][2] * dD[2]);
    *xi = -(Hinv[2][0] * dD[0] + Hinv[2][1] * dD[1] + Hinv[2][2] * dD[2]);
    return 1;
}
/* CONFIRMED structural match: OpenSIFT's interp_contr (FtInterpContr). */
static double interp_contr(FImage ***dog, int o, int i, int r, int c,
                            double xi, double xr, double xc)
{
    double dD[3];
    deriv_3D(dog, o, i, r, c, dD);
    return img_get(dog[o][i], r, c) + 0.5 * (dD[0] * xc + dD[1] * xr + dD[2] * xi);
}
/* CONFIRMED structural match: OpenSIFT's is_too_edge_like. */
static int is_too_edge_like(FImage *dogImg, int r, int c, int curv_thr)
{
    double d = img_get(dogImg, r, c);
    double dxx = img_get(dogImg, r, c + 1) + img_get(dogImg, r, c - 1) - 2 * d;
    double dyy = img_get(dogImg, r + 1, c) + img_get(dogImg, r - 1, c) - 2 * d;
    double dxy = (img_get(dogImg, r + 1, c + 1) - img_get(dogImg, r + 1, c - 1)
                - img_get(dogImg, r - 1, c + 1) + img_get(dogImg, r - 1, c - 1)) / 4.0;
    double tr = dxx + dyy;
    double det = dxx * dyy - dxy * dxy;
    if (det <= 0) return 1;
    if (tr * tr / det < (curv_thr + 1.0) * (curv_thr + 1.0) / curv_thr) return 0;
    return 1;
}
static int is_extremum(FImage ***dog, int o, int i, int r, int c)
{
    double val = img_get(dog[o][i], r, c);
    int di, dj, dk;
    if (val > 0) {
        for (di = -1; di <= 1; di++)
            for (dj = -1; dj <= 1; dj++)
                for (dk = -1; dk <= 1; dk++)
                    if (val < img_get(dog[o][i + di], r + dj, c + dk)) return 0;
    } else {
        for (di = -1; di <= 1; di++)
            for (dj = -1; dj <= 1; dj++)
                for (dk = -1; dk <= 1; dk++)
                    if (val > img_get(dog[o][i + di], r + dj, c + dk)) return 0;
    }
    return 1;
}

/* CONFIRMED structural match: OpenSIFT's scale_space_extrema (FtScaleSpaceExtrema),
 * inlining is_extremum/interp_extremum as the real binary appears to. */
static void scale_space_extrema(FImage ***dog, int octvs, int intvls,
                                 double contr_thr, int curv_thr, KpList *out)
{
    double prelim_contr_thr = 0.5 * contr_thr / intvls;
    int o, i, r, c;
    for (o = 0; o < octvs; o++) {
        int H = dog[o][0]->rows, W = dog[o][0]->cols;
        /* CONFIRMED structural match: OpenSIFT's per-octave feature_mat
         * dedup bitmask (audit fix -- previously dropped). Prevents the
         * same final (r,c,intvl) location from being pushed twice if two
         * different search starting points converge to it during
         * interp_extremum's iterative refinement. */
        unsigned long *featureMat = calloc((size_t)H * W, sizeof(unsigned long));
        for (i = 1; i <= intvls; i++)
            for (r = SIFT_IMG_BORDER; r < H - SIFT_IMG_BORDER; r++)
                for (c = SIFT_IMG_BORDER; c < W - SIFT_IMG_BORDER; c++) {
                    if (fabs(img_get(dog[o][i], r, c)) <= prelim_contr_thr) continue;
                    if (!is_extremum(dog, o, i, r, c)) continue;

                    int oo = o, ii = i, rr = r, cc = c, step;
                    double xi = 0, xr = 0, xc = 0;
                    int converged = 0;
                    for (step = 0; step < SIFT_MAX_INTERP_STEPS; step++) {
                        if (!interp_step(dog, oo, ii, rr, cc, &xi, &xr, &xc)) break;
                        if (fabs(xi) < 0.5 && fabs(xr) < 0.5 && fabs(xc) < 0.5) { converged = 1; break; }
                        cc += (int)lround(xc);
                        rr += (int)lround(xr);
                        ii += (int)lround(xi);
                        if (ii < 1 || ii > intvls || cc < SIFT_IMG_BORDER || rr < SIFT_IMG_BORDER ||
                            cc >= dog[oo][0]->cols - SIFT_IMG_BORDER || rr >= dog[oo][0]->rows - SIFT_IMG_BORDER)
                            break;
                    }
                    if (!converged) continue;

                    double contr = interp_contr(dog, oo, ii, rr, cc, xi, xr, xc);
                    if (fabs(contr) < contr_thr / intvls) continue;
                    if (is_too_edge_like(dog[oo][ii], rr, cc, curv_thr)) continue;

                    /* ii is always <= intvls (<=8 for any sane intvls), so
                     * OpenSIFT's "intvl > sizeof(unsigned long)" branch is
                     * dead code here too -- the bitmask check always
                     * applies, matching real-world behavior. */
                    unsigned long bit = 1UL << (ii - 1);
                    if (featureMat[W * rr + cc] & bit) continue;
                    featureMat[W * rr + cc] |= bit;

                    FocalKeypoint kp = {0};
                    kp.x = (float)((cc + xc) * pow(2.0, oo));
                    kp.y = (float)((rr + xr) * pow(2.0, oo));
                    kp.local_x = (float)(cc + xc);
                    kp.local_y = (float)(rr + xr);
                    kp.octv = oo; kp.intvl = ii; kp.r = rr; kp.c = cc;
                    kp.subintvl = (float)xi;
                    kp_push(out, kp);
                }
        free(featureMat);
    }
}

/* CONFIRMED structural match: OpenSIFT's calc_feature_scales. */
static void calc_feature_scales(KpList *l, double sigma, int intvls)
{
    int i;
    for (i = 0; i < l->n; i++) {
        FocalKeypoint *f = &l->items[i];
        double intvl = f->intvl + f->subintvl;
        f->scl = (float)(sigma * pow(2.0, f->octv + intvl / intvls));
        f->scl_octv = (float)(sigma * pow(2.0, intvl / intvls));
    }
}
/* CONFIRMED structural match: OpenSIFT's adjust_for_img_dbl. */
static void adjust_for_img_dbl(KpList *l)
{
    int i;
    for (i = 0; i < l->n; i++) {
        l->items[i].x /= (float)FOCAL_DBL_SCALE;
        l->items[i].y /= (float)FOCAL_DBL_SCALE;
        l->items[i].scl /= (float)FOCAL_DBL_SCALE;
    }
}

/* CONFIRMED structural match: OpenSIFT's calc_grad_mag_ori/ori_hist/
 * smooth_ori_hist/dominant_ori/add_good_ori_features. */
static int calc_grad_mag_ori(FImage *im, int r, int c, double *mag, double *ori)
{
    if (r > 0 && r < im->rows - 1 && c > 0 && c < im->cols - 1) {
        double dx = img_get(im, r, c + 1) - img_get(im, r, c - 1);
        double dy = img_get(im, r - 1, c) - img_get(im, r + 1, c);
        *mag = sqrt(dx * dx + dy * dy);
        *ori = atan2(dy, dx);
        return 1;
    }
    return 0;
}
static double *ori_hist(FImage *im, int r, int c, int n, int rad, double sigma)
{
    double *hist = calloc((size_t)n, sizeof(double));
    double exp_denom = 2.0 * sigma * sigma;
    double mag, ori;
    int i, j;
    for (i = -rad; i <= rad; i++)
        for (j = -rad; j <= rad; j++)
            if (calc_grad_mag_ori(im, r + i, c + j, &mag, &ori)) {
                double w = exp(-(i * i + j * j) / exp_denom);
                int bin = (int)lround(n * (ori + M_PI) / (2.0 * M_PI));
                bin = (bin < n) ? bin : 0;
                hist[bin] += w * mag;
            }
    return hist;
}
static void smooth_ori_hist(double *hist, int n)
{
    double h0 = hist[0], prev = hist[n - 1];
    int i;
    for (i = 0; i < n; i++) {
        double tmp = hist[i];
        hist[i] = 0.25 * prev + 0.5 * hist[i] + 0.25 * ((i + 1 == n) ? h0 : hist[i + 1]);
        prev = tmp;
    }
}
static double dominant_ori(double *hist, int n)
{
    double omax = hist[0]; int i;
    for (i = 1; i < n; i++) if (hist[i] > omax) omax = hist[i];
    return omax;
}
static double interp_hist_peak(double l, double c, double r)
{
    return 0.5 * ((l - r) / (l - 2.0 * c + r));
}
static void add_good_ori_features(KpList *l, double *hist, int n, double mag_thr, FocalKeypoint base)
{
    int i;
    for (i = 0; i < n; i++) {
        int lft = (i == 0) ? n - 1 : i - 1;
        int rgt = (i + 1) % n;
        if (hist[i] > hist[lft] && hist[i] > hist[rgt] && hist[i] >= mag_thr) {
            double bin = i + interp_hist_peak(hist[lft], hist[i], hist[rgt]);
            bin = (bin < 0) ? n + bin : (bin >= n) ? bin - n : bin;
            FocalKeypoint nf = base;
            nf.ori = (float)((2.0 * M_PI * bin) / n - M_PI);
            kp_push(l, nf);
        }
    }
}
/* CONFIRMED structural match: OpenSIFT's calc_feature_oris. Note: unlike
 * OpenSIFT (which pops from the front and re-pushes with orientation set),
 * this builds a fresh oriented list from the localized-keypoint list. */
static void calc_feature_oris(KpList *localized, FImage ***gauss_pyr, KpList *oriented)
{
    int i;
    for (i = 0; i < localized->n; i++) {
        FocalKeypoint *f = &localized->items[i];
        double *hist = ori_hist(gauss_pyr[f->octv][f->intvl], f->r, f->c,
                                 SIFT_ORI_HIST_BINS,
                                 (int)lround(SIFT_ORI_RADIUS * f->scl_octv),
                                 SIFT_ORI_SIG_FCTR * f->scl_octv);
        int j;
        for (j = 0; j < SIFT_ORI_SMOOTH_PASSES; j++) smooth_ori_hist(hist, SIFT_ORI_HIST_BINS);
        double omax = dominant_ori(hist, SIFT_ORI_HIST_BINS);
        add_good_ori_features(oriented, hist, SIFT_ORI_HIST_BINS, omax * SIFT_ORI_PEAK_RATIO, *f);
        free(hist);
    }
}

/* CONFIRMED algorithm structure, exact data tables (FtMfbDescriptors).
 * Descriptor: 8x UINT32 = 256 bits, one per entry in g_mode_pairs. */
static void compute_binary_descriptor(FImage ***gauss_pyr, const FocalKeypoint *f,
                                       unsigned int desc[8])
{
    FImage *im = gauss_pyr[f->octv][f->intvl];
    /* CONFIRMED via ground-truth sample-array diffing on TWO independent
     * real keypoints at very different orientations (research/PROTOCOL.md):
     * the real algorithm's steered sampling uses theta = -f->ori (i.e.
     * cos_o=cos(ori), sin_o=-sin(ori)), not ori+PI. An earlier version of
     * this fix (negate both cos_o and sin_o, equivalent to rotating by
     * ori+PI) was validated against only one keypoint whose orientation
     * (~1.597 rad, near PI/2) happens to make ori+PI and -ori nearly
     * numerically coincide -- masking the error. A second keypoint at a
     * generic orientation (-0.315 rad) exposed it: ori+PI gave sum-sq
     * error ~78,291 and Hamming 185/256, while a systematic search over
     * rotation conventions found theta=-ori gives sum-sq error ~3,368 and
     * Hamming 29/256 -- and re-checking the first keypoint with theta=-ori
     * also gives Hamming 28/256 (previously 29/256 under the wrong
     * convention). theta=-ori is confirmed correct for both. */
    float cos_o = cosf(f->ori);
    float sin_o = -sinf(f->ori);
    float samples[FOCAL_NUM_SAMPLE_POINTS];
    int i;
    for (i = 0; i < FOCAL_NUM_SAMPLE_POINTS; i++) {
        float dx = g_coordinare_pairs[i][0], dy = g_coordinare_pairs[i][1];
        /* Sample in gauss_pyr[octv][intvl]'s OWN pixel grid via local_x/y
         * (not the global x/y, which have octave-scale and img_dbl
         * folded in and would index the wrong pyramid level's pixels). */
        float rx = f->local_x + (dx * cos_o - dy * sin_o);
        float ry = f->local_y + (dx * sin_o + dy * cos_o);
        int ix = (int)lroundf(rx), iy = (int)lroundf(ry);
        samples[i] = (ix >= 0 && ix < im->cols && iy >= 0 && iy < im->rows)
                     ? img_get(im, iy, ix) : 0.0f;
    }
    memset(desc, 0, 8 * sizeof(unsigned int));
    for (i = 0; i < FOCAL_NUM_DESCRIPTOR_BITS; i++) {
        int a = g_mode_pairs[i][0], b = g_mode_pairs[i][1];
        /* CONFIRMED via ground-truth diffing: comparison direction is
         * sample[a] > sample[b] (not <). Combined with the ori+PI rotation
         * fix above, this brought a known real keypoint's descriptor to
         * Hamming distance 29/256 against real ground truth (down from
         * 225/256 with < and the un-rotated convention) -- a clear,
         * decisive match, not noise. See research/PROTOCOL.md. */
        if (samples[a] > samples[b]) desc[i / 32] |= (1u << (i % 32));
    }
}

typedef struct {
    float x, y, ori;
    unsigned int desc[8];
} FocalFeature;

/* Top-level entry point: runs the full detect+describe pipeline on one
 * 8-bit grayscale image, mirroring FtGetMfbFeatures (detection identical
 * to FtGetMfsFeatures, descriptor = FtMfbDescriptors). Returns a malloc'd
 * array via *out_features (caller frees); returns feature count. */
int focal_extract_features(const unsigned char *img, int rows, int cols,
                            int octaves, FocalFeature **out_features)
{
    /* CONFIRMED via real .so ground truth: pad the native capture into the
     * algorithm's fixed 96x96 working canvas (centered, zero-padded --
     * the aspect-ratio-preserving choice; a stretch would distort ridge
     * geometry, see PROTOCOL.md) before running detection at all. */
    int canvasDim = FOCAL_WORKING_CANVAS;
    unsigned char *padded = NULL;
    const unsigned char *detectInput = img;
    int detectRows = rows, detectCols = cols;
    if (rows != canvasDim || cols != canvasDim) {
        padded = calloc((size_t)canvasDim * canvasDim, 1);
        int padRows = (canvasDim - rows) / 2, padCols = (canvasDim - cols) / 2;
        int r;
        for (r = 0; r < rows && r + padRows < canvasDim; r++)
            memcpy(padded + (r + padRows) * canvasDim + padCols, img + r * cols,
                   (size_t)(cols < canvasDim - padCols ? cols : canvasDim - padCols));
        detectInput = padded;
        detectRows = detectCols = canvasDim;
    }

    FImage *base = create_init_img(detectInput, detectRows, detectCols, FOCAL_IMG_DBL, FOCAL_SIGMA);
    free(padded);
    FImage ***gpyr = build_gauss_pyr(base, octaves, FOCAL_INTVLS, FOCAL_SIGMA);
    FImage ***dpyr = build_dog_pyr(gpyr, octaves, FOCAL_INTVLS);

    KpList localized = {0}, oriented = {0};
    scale_space_extrema(dpyr, octaves, FOCAL_INTVLS, FOCAL_CONTR_THR, FOCAL_CURV_THR, &localized);
    calc_feature_scales(&localized, FOCAL_SIGMA, FOCAL_INTVLS);
    if (FOCAL_IMG_DBL) adjust_for_img_dbl(&localized);
    calc_feature_oris(&localized, gpyr, &oriented);

    FocalFeature *feats = malloc((size_t)oriented.n * sizeof(FocalFeature));
    int i;
    for (i = 0; i < oriented.n; i++) {
        FocalKeypoint *k = &oriented.items[i];
        feats[i].x = k->x; feats[i].y = k->y; feats[i].ori = k->ori;
        /* local_x/local_y are untouched by adjust_for_img_dbl (only x/y/scl
         * are), so they still correctly index gauss_pyr[octv][intvl]. */
        compute_binary_descriptor(gpyr, k, feats[i].desc);
    }

    free(localized.items);
    free(oriented.items);
    free_pyr(dpyr, octaves, FOCAL_INTVLS + 2);
    free_pyr(gpyr, octaves, FOCAL_INTVLS + 3);
    img_free(base);

    *out_features = feats;
    return oriented.n;
}

int focal_hamming_distance(const unsigned int a[8], const unsigned int b[8])
{
    int i, d = 0;
    for (i = 0; i < 8; i++) d += __builtin_popcount(a[i] ^ b[i]);
    return d;
}

/* DIAGNOSTIC-ONLY test hooks (tools/step1_reproduce_samples.c) -- expose
 * the internal Gaussian pyramid and float-image accessor so an external
 * tool can reproduce compute_binary_descriptor's exact sampling stage for
 * an arbitrary externally-supplied keypoint, without needing our own
 * detector to have found a keypoint there. Not used by the real pipeline. */
FImage ***focal_debug_get_pyramid(const unsigned char *img, int rows, int cols,
                                   int octaves, int *outIntvlsPlus3)
{
    FImage *base = create_init_img(img, rows, cols, FOCAL_IMG_DBL, FOCAL_SIGMA);
    FImage ***gpyr = build_gauss_pyr(base, octaves, FOCAL_INTVLS, FOCAL_SIGMA);
    img_free(base);
    *outIntvlsPlus3 = FOCAL_INTVLS + 3;
    return gpyr;
}

float focal_debug_img_get(FImage *im, int r, int c)
{
    return img_get(im, r, c);
}
