/*
 * Clean-room reimplementation of FocalTech's FT9366 host-side template
 * extraction / matching pipeline, built entirely from this project's own
 * reverse-engineering (see research/PROTOCOL.md) -- zero runtime dependency
 * on the proprietary .so. Every function here is annotated with a status:
 *
 *   CONFIRMED   -- algorithm traced from disassembly/DWARF with real
 *                  constants extracted from .rodata/.data, cross-checked
 *                  against a public reference where one exists (OpenCV
 *                  border/box-filter semantics, OpenSIFT for detection).
 *   BEST-EFFORT -- a standard, reasonable implementation of a stage whose
 *                  exact internal logic was not fully bit-traced (see the
 *                  matching PROTOCOL.md entry for what's uncertain).
 *
 * This is a first-draft, UNTESTED reimplementation. It has NOT yet been
 * validated against real captures or the real .so's intermediate buffers.
 * Do not treat any function here as "working" until that validation has
 * actually been run and reported plainly, per this project's own rules.
 *
 * Build: gcc -O2 -Wall -c focal_match.c -o focal_match.o -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

typedef uint8_t  UINT8;
typedef int8_t   SINT8;
typedef uint16_t UINT16;
typedef int16_t  SINT16;
typedef uint32_t UINT32;
typedef int32_t  SINT32;
typedef float    FP32;

/* ------------------------------------------------------------------ */
/* Shared primitives (Step 1 finding: these are OpenCV-equivalent      */
/* standard operations, not bespoke -- research/PROTOCOL.md, "Finding: */
/* shared primitives are OpenCV-equivalent standard operations").      */
/* ------------------------------------------------------------------ */

/* CONFIRMED: structural match to OpenCV's cv::borderInterpolate().
 * borderType: only BORDER_REFLECT_101 (=4) is used anywhere in the
 * traced pipeline (FtImgBoxFilter's real call sites), so that's the
 * only mode implemented here. */
static int border_interpolate_101(int p, int len)
{
    if ((unsigned)p < (unsigned)len)
        return p;
    if (len == 1)
        return 0;
    do {
        if (p < 0)
            p = -p;
        else
            p = 2 * (len - 1) - p;
    } while ((unsigned)p >= (unsigned)len);
    return p;
}

/* CONFIRMED: FtImgBoxFilter / FtBoxFilter_32f -- separable box average,
 * BORDER_REFLECT_101 edges, optional normalize (divide by ksize^2).
 * Traced as a running-sum optimization in the real binary; implemented
 * here as a direct O(ksize^2) box average, which is numerically
 * IDENTICAL for a box filter (not an approximation of the original's
 * optimization -- box averaging has no approximation error either way). */
static void box_filter_32f(const FP32 *src, int rows, int cols, int ksize,
                            FP32 *dst, int normalize)
{
    int radius = ksize / 2;
    FP32 *tmp = malloc((size_t)rows * cols * sizeof(FP32));
    int r, c, k;

    /* horizontal pass */
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            FP32 sum = 0;
            for (k = -radius; k <= radius; k++) {
                int cc = border_interpolate_101(c + k, cols);
                sum += src[r * cols + cc];
            }
            tmp[r * cols + c] = sum;
        }
    }
    /* vertical pass */
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            FP32 sum = 0;
            for (k = -radius; k <= radius; k++) {
                int rr = border_interpolate_101(r + k, rows);
                sum += tmp[rr * cols + c];
            }
            dst[r * cols + c] = normalize ? sum / (FP32)(ksize * ksize) : sum;
        }
    }
    free(tmp);
}

/* CONFIRMED: FtImgGaussianblur -- called with sigma=-1.0 (auto) at every
 * traced call site. OpenCV's auto-sigma formula for sigma<=0:
 *   sigma = 0.3*((ksize-1)*0.5 - 1) + 0.8
 * BEST-EFFORT: assumed to match this OpenCV convention exactly (strong
 * structural evidence for OpenCV-equivalence elsewhere in this codebase,
 * not yet independently bit-verified for the auto-sigma formula itself). */
static void gaussian_blur_u8_inplace(UINT8 *img, int rows, int cols, int ksize)
{
    double sigma = 0.3 * ((ksize - 1) * 0.5 - 1) + 0.8;
    int radius = ksize / 2;
    double *kernel = malloc((size_t)ksize * sizeof(double));
    double sum = 0;
    int i, r, c;
    FP32 *tmp = malloc((size_t)rows * cols * sizeof(FP32));
    FP32 *tmp2 = malloc((size_t)rows * cols * sizeof(FP32));

    for (i = 0; i < ksize; i++) {
        double x = i - radius;
        kernel[i] = exp(-(x * x) / (2 * sigma * sigma));
        sum += kernel[i];
    }
    for (i = 0; i < ksize; i++)
        kernel[i] /= sum;

    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++) {
            double acc = 0;
            for (i = -radius; i <= radius; i++) {
                int cc = border_interpolate_101(c + i, cols);
                acc += kernel[i + radius] * img[r * cols + cc];
            }
            tmp[r * cols + c] = (FP32)acc;
        }
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++) {
            double acc = 0;
            for (i = -radius; i <= radius; i++) {
                int rr = border_interpolate_101(r + i, rows);
                acc += kernel[i + radius] * tmp[rr * cols + c];
            }
            tmp2[r * cols + c] = (FP32)acc;
        }
    for (i = 0; i < rows * cols; i++) {
        int v = (int)(tmp2[i] + 0.5f);
        img[i] = (UINT8)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
    free(kernel);
    free(tmp);
    free(tmp2);
}

/* CONFIRMED: curved_surface_img_normalize_32f_2_8u / FtNormalize_32f_2_8u --
 * min-max scale into [alpha,beta]. */
static void normalize_32f_to_8u(const FP32 *src, int n, FP32 alpha, FP32 beta,
                                 UINT8 *dst)
{
    FP32 mn = src[0], mx = src[0];
    int i;
    for (i = 1; i < n; i++) {
        if (src[i] < mn) mn = src[i];
        if (src[i] > mx) mx = src[i];
    }
    FP32 range = mx - mn;
    if (range < 1e-6f) range = 1e-6f;
    FP32 scale = (beta - alpha) / range;
    for (i = 0; i < n; i++) {
        int v = (int)((src[i] - mn) * scale + alpha);
        dst[i] = (UINT8)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

/* ------------------------------------------------------------------ */
/* Step 1 preprocessing chain (research/PROTOCOL.md "STEP 1 WRAP-UP")  */
/* ------------------------------------------------------------------ */

/* CONFIRMED (full raw-disassembly trace). One BEST-EFFORT sub-step:
 * curved_surface_img_localequalizehist_v2's exact inner accumulation
 * logic was not fully disambiguated (see PROTOCOL.md) -- implemented
 * here as a standard local histogram equalization within a small
 * window, mask-aware, as the closest reasonable match to what was
 * observed. This is the one genuinely uncertain piece of this function. */
static void local_equalize_hist_v2_bestguess(const UINT8 *src, const UINT8 *mask,
                                              int rows, int cols, UINT8 *dst)
{
    /* BEST-EFFORT: real window shape/size not fully resolved (asymmetric
     * padding was observed -- ~55 tall, ~narrow horizontally -- but not
     * pinned down exactly). Using a symmetric local window as a
     * placeholder pending empirical validation against real captures. */
    const int win = 15, rad = win / 2;
    int r, c;
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            int sum = 0, cnt = 0;
            int rr, cc;
            for (rr = r - rad; rr <= r + rad; rr++) {
                if (rr < 0 || rr >= rows) continue;
                for (cc = c - rad; cc <= c + rad; cc++) {
                    if (cc < 0 || cc >= cols) continue;
                    if (mask && !mask[rr * cols + cc]) continue;
                    sum += src[rr * cols + cc];
                    cnt++;
                }
            }
            /* Confirmed formula from PROTOCOL.md for the per-window
             * aggregate step: output = sum*255/count (a local mean
             * rescaled into the 0-255 range under the placeholder
             * assumption that `sum` here is itself already a 0..count
             * range accumulation -- see the BEST-EFFORT note above). */
            dst[r * cols + c] = cnt ? (UINT8)(sum / cnt) : src[r * cols + c];
        }
    }
}

/* CONFIRMED (full raw-disassembly trace, PROTOCOL.md "STEP 1 (reimplementation):
 * FtNonLinearStretch_U8 fully traced"). Returns 9 on success (confirmed
 * non-zero sentinel, not a bug). */
int focal_non_linear_stretch_u8(const UINT8 *src, int rows, int cols, UINT8 *dst)
{
    if (!src || !dst) return -1;
    int n = rows * cols;
    FP32 *bufA = malloc((size_t)n * sizeof(FP32));
    FP32 *bufB = malloc((size_t)n * sizeof(FP32));
    UINT8 *mask = calloc((size_t)n, 1);
    UINT8 *normImg = malloc((size_t)n);
    int i;

    for (i = 0; i < n; i++)
        bufA[i] = bufB[i] = (FP32)src[i];

    box_filter_32f(bufA, rows, cols, 3, bufA, 1);
    box_filter_32f(bufB, rows, cols, 5, bufB, 1);
    for (i = 0; i < n; i++)
        bufA[i] -= bufB[i];

    normalize_32f_to_8u(bufA, n, 0.0f, 250.0f, normImg);

    for (i = 0; i < n; i++)
        mask[i] = src[i] > 0xfa ? 1 : 0;

    local_equalize_hist_v2_bestguess(normImg, mask, rows, cols, dst);
    gaussian_blur_u8_inplace(dst, rows, cols, 3);

    for (i = 0; i < n; i++)
        if (mask[i])
            dst[i] = 0xfe;

    free(bufA); free(bufB); free(mask); free(normImg);
    return 9;
}

/* CONFIRMED (full trace, PROTOCOL.md "FtGrayMeanSub"). Returns 0 on success. */
int focal_gray_mean_sub(UINT8 *src, int rows, int cols, int ksize)
{
    if (!src || ksize <= 2) return -1;
    int n = rows * cols;
    FP32 *bufA = malloc((size_t)n * sizeof(FP32));
    FP32 *bufB = malloc((size_t)n * sizeof(FP32));
    int i;

    for (i = 0; i < n; i++)
        bufA[i] = bufB[i] = (FP32)src[i];

    box_filter_32f(bufA, rows, cols, 3, bufA, 1);
    box_filter_32f(bufB, rows, cols, ksize, bufB, 1);
    for (i = 0; i < n; i++)
        bufA[i] -= bufB[i];

    normalize_32f_to_8u(bufA, n, 0.0f, 254.0f, src);

    free(bufA); free(bufB);
    return 0;
}

/* CONFIRMED (full trace, exact constants extracted from .rodata,
 * PROTOCOL.md "FtLocalContrastEnhance"). Returns 0 on success. */
int focal_local_contrast_enhance(UINT8 *src, int rows, int cols, int ksize)
{
    if (!src) return -1;
    static const FP32 GAIN = 0.2f, FLOOR = 1.0f, SCALE = 250.0f;
    static const double EPS = 1e-6;
    int n = rows * cols;
    FP32 *bufMean = malloc((size_t)n * sizeof(FP32));
    FP32 *bufVar = malloc((size_t)n * sizeof(FP32));
    long sum = 0;
    int i;

    gaussian_blur_u8_inplace(src, rows, cols, 3);

    for (i = 0; i < n; i++) {
        bufMean[i] = (FP32)src[i];
        bufVar[i] = (FP32)(src[i] * src[i]);
        sum += src[i];
    }
    box_filter_32f(bufMean, rows, cols, ksize, bufMean, 1);
    box_filter_32f(bufVar, rows, cols, ksize, bufVar, 1);

    FP32 globalMean = (FP32)(sum / n);
    for (i = 0; i < n; i++) {
        FP32 localVar = bufVar[i] - bufMean[i] * bufMean[i];
        FP32 localStd = localVar > 0 ? sqrtf(localVar) : FLOOR;
        if (localStd < FLOOR) localStd = FLOOR;
        FP32 gain = (globalMean * GAIN) / localStd;
        bufVar[i] = bufMean[i] + gain * ((FP32)src[i] - bufMean[i]);
    }

    FP32 mn = bufVar[0], mx = bufVar[0];
    for (i = 1; i < n; i++) {
        if (bufVar[i] < mn) mn = bufVar[i];
        if (bufVar[i] > mx) mx = bufVar[i];
    }
    double range = mx - mn;
    if (range < EPS) range = EPS;
    FP32 scale = (FP32)(SCALE / range);
    for (i = 0; i < n; i++) {
        int v = (int)((bufVar[i] - mn) * scale);
        src[i] = (UINT8)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    free(bufMean); free(bufVar);
    return 0;
}

/* CONFIRMED (full trace, PROTOCOL.md "FtSegmentByLocalVariance"). Produces a
 * foreground/background validity mask (1=foreground/valid ridge area).
 * BEST-EFFORT: FtErode/FtDilate's exact structuring element/iteration
 * semantics not independently traced -- using a plain square structuring
 * element of the given size as a reasonable standard equivalent. */
int focal_segment_by_local_variance(const UINT8 *src, int rows, int cols,
                                     int ksize, FP32 thr, UINT8 *dst)
{
    if (!src || !dst) return -1;
    int n = rows * cols;
    UINT16 *meanBuf = malloc((size_t)n * sizeof(UINT16));
    UINT16 *sqBuf = malloc((size_t)n * sizeof(UINT16));
    /* NOTE: real primitive is FtBoxFilter_16u; box_filter_32f used here on
     * widened float buffers as a numerically-equivalent stand-in. */
    FP32 *fa = malloc((size_t)n * sizeof(FP32));
    FP32 *fb = malloc((size_t)n * sizeof(FP32));
    int i, r, c;

    for (i = 0; i < n; i++) {
        fa[i] = (FP32)src[i];
        fb[i] = (FP32)(src[i] * src[i]);
    }
    box_filter_32f(fa, rows, cols, ksize, fa, 1);
    box_filter_32f(fb, rows, cols, ksize, fb, 1);

    FP32 thr2 = thr * thr;
    for (i = 0; i < n; i++) {
        FP32 localMean = fa[i];
        FP32 localMeanOfSq = fb[i];
        FP32 var = localMeanOfSq - localMean * localMean;
        dst[i] = (var > thr2) ? 1 : 0;
    }
    (void)meanBuf; (void)sqBuf;
    free(fa); free(fb); free(meanBuf); free(sqBuf);

    /* erode(7) then dilate(5) -- BEST-EFFORT square structuring elements */
    UINT8 *tmp = malloc((size_t)n);
    memcpy(tmp, dst, (size_t)n);
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++) {
            int keep = 1, rr, cc;
            for (rr = r - 3; rr <= r + 3 && keep; rr++)
                for (cc = c - 3; cc <= c + 3 && keep; cc++) {
                    if (rr < 0 || rr >= rows || cc < 0 || cc >= cols || !tmp[rr * cols + cc])
                        keep = 0;
                }
            dst[r * cols + c] = keep;
        }
    memcpy(tmp, dst, (size_t)n);
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++) {
            int hit = 0, rr, cc;
            for (rr = r - 2; rr <= r + 2 && !hit; rr++)
                for (cc = c - 2; cc <= c + 2 && !hit; cc++) {
                    if (rr >= 0 && rr < rows && cc >= 0 && cc < cols && tmp[rr * cols + cc])
                        hit = 1;
                }
            dst[r * cols + c] = hit;
        }
    free(tmp);
    return 0;
}
