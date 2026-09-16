/*
 * DIAGNOSTIC-ONLY tool. SPA-smoothing variant of ground_truth_calcsimscore_batch.c.
 *
 * Live-execution tracing of the REAL FtGetTemplateForEnroll (a genuinely
 * distinct, previously-untested enrollment-time function -- see
 * research/PROTOCOL.md "Windows-vs-Linux discrepancy investigation")
 * showed its real, robust execution path is:
 *   InitSPAImageSize(96,96) -> InitSPAMaskRadius(5) -> InitSPAImpactFactors(0)
 *   -> FtSpaSmooth(canvas, impactFactor=20) -> FtGetMfbFeatures -> FtGenBinImg
 * CONFIRMED identical and robust across 3 independent real captures (exact
 * same functions, exact same parameters, live gdb tracing). Critically,
 * this does NOT include FtNonLinearStretch_U8/FtGrayMeanSub/
 * FtBadPixselDetect/FtLocalContrastEnhance/f9395_image_enhance/
 * FtResize_8u/FtSegmentByLocalVariance at all for this call path -- an
 * earlier session's "confirmed pipeline order" (from static call-graph
 * analysis, research/PROTOCOL.md "FtGetTemplate pipeline mapped") is
 * contradicted by this live-execution evidence for this entry point.
 * This tool adds the ONE real, confirmed, exact-parameter gap (SPA
 * smoothing) that every prior ground-truth extraction this project has
 * ever done was missing, and re-tests real separation with it included.
 *
 * NEVER shipped, NEVER used at runtime -- diagnostic-only.
 *
 * Usage: ground_truth_calcsimscore_batch_spa <so_path> label1:file1.raw label2:file2.raw ...
 *   Files sharing the same label are treated as the SAME physical finger.
 *
 * Build: gcc -O0 -g -Wall ground_truth_calcsimscore_batch_spa.c -o ground_truth_calcsimscore_batch_spa -ldl -lm
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
#define OFF_FtGenBinImg              0x108df0
#define OFF_FtSegmentByLocalVariance 0xe3600
#define OFF_FtCalcSimScore           0x115870
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
typedef UINT16 (*FtGenBinImg_fn)(ST_IplImage *, UINT64 **, UINT16 *);
typedef int (*FtSegmentByLocalVariance_fn)(UINT8 *, SINT32, SINT32, SINT32, FP32, UINT8 *);
typedef float (*FtCalcSimScore_fn)(UINT8 *, UINT8 *, UINT8 *, UINT8 *, SINT32, SINT32, FP32 *, UINT16 *);
typedef void (*InitSPAImageSize_fn)(UINT16, UINT16);
typedef void (*InitSPAMaskRadius_fn)(UINT16);
typedef void (*InitSPAImpactFactors_fn)(FP32);
typedef unsigned char (*FtSpaSmooth_fn)(UINT8 *, UINT16);

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
static unsigned char *pad_to_canvas(const unsigned char *tight, int rows, int cols, int canvas)
{
    unsigned char *canvasBuf = calloc(1, (size_t)canvas * canvas);
    int padRows = (canvas - rows) / 2, padCols = (canvas - cols) / 2, r;
    for (r = 0; r < rows; r++)
        memcpy(canvasBuf + (r + padRows) * canvas + padCols, tight + r * cols, (size_t)cols);
    return canvasBuf;
}
static void unpack_bits_to_bytes(const UINT64 *bits, UINT8 *out, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        UINT64 word = bits[i / 64];
        int bit = (word >> (i % 64)) & 1;
        out[i] = bit ? 1 : 0;
    }
}

typedef struct {
    char label[64];
    char path[256];
    UINT8 *canvasBuf, *mask, *bin;
    ST_Feature *flat; /* feat1+feat2 combined */
    int n;
} RealCapture;

static void load_real_capture(uintptr_t bias, const char *label, const char *path, int canvas, RealCapture *out)
{
    strncpy(out->label, label, sizeof(out->label) - 1);
    strncpy(out->path, path, sizeof(out->path) - 1);
    const int rows = 80, cols = 64;
    unsigned char *tight = load_raw_be16_as_u8(path, rows * cols);
    out->canvasBuf = pad_to_canvas(tight, rows, cols, canvas);
    free(tight);

    /* CONFIRMED via live gdb tracing of the real FtGetTemplateForEnroll
     * (robust across 3 independent captures, identical parameters every
     * time): the real pipeline applies SPA smoothing to the canvas BEFORE
     * detection/binarization. Every ground-truth extraction this project
     * has done before this fix skipped this step entirely. */
    InitSPAImageSize_fn spaInitSize = (InitSPAImageSize_fn)(bias + OFF_InitSPAImageSize);
    InitSPAMaskRadius_fn spaInitRadius = (InitSPAMaskRadius_fn)(bias + OFF_InitSPAMaskRadius);
    InitSPAImpactFactors_fn spaInitImpact = (InitSPAImpactFactors_fn)(bias + OFF_InitSPAImpactFactors);
    FtSpaSmooth_fn spaSmooth = (FtSpaSmooth_fn)(bias + OFF_FtSpaSmooth);
    spaInitSize((UINT16)canvas, (UINT16)canvas);
    spaInitRadius(5);
    spaInitImpact(0.0f);
    spaSmooth(out->canvasBuf, 20);

    FtSegmentByLocalVariance_fn segFn = (FtSegmentByLocalVariance_fn)(bias + OFF_FtSegmentByLocalVariance);
    out->mask = malloc((size_t)canvas * canvas);
    segFn(out->canvasBuf, canvas, canvas, 9, 12.0f, out->mask);

    FtGenBinImg_fn binFn = (FtGenBinImg_fn)(bias + OFF_FtGenBinImg);
    ST_IplImage iplImage = {
        .depth = 8, .width = canvas, .height = canvas,
        .imageSize = canvas * canvas, .widthStep = canvas, .imageData = out->canvasBuf
    };
    UINT64 *bitArr = NULL;
    UINT16 arrLen = 0;
    binFn(&iplImage, &bitArr, &arrLen);
    out->bin = malloc((size_t)canvas * canvas);
    if (bitArr) unpack_bits_to_bytes(bitArr, out->bin, canvas * canvas);
    else memset(out->bin, 0, (size_t)canvas * canvas);

    FtGetMfbFeatures_fn featFn = (FtGetMfbFeatures_fn)(bias + OFF_FtGetMfbFeatures);
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
    out->n = ret2.nMaxExtremum + ret2.nMinExtremum;
    out->flat = malloc((size_t)out->n * sizeof(ST_Feature));
    int k = 0, i;
    for (i = 0; i < ret2.nMaxExtremum; i++) out->flat[k++] = feat1[i];
    for (i = 0; i < ret2.nMinExtremum; i++) out->flat[k++] = feat2[i];
    free(validMask); free(badPixMask);
}

static int hamming(const UINT32 a[8], const UINT32 b[8])
{
    int i, d = 0;
    for (i = 0; i < 8; i++) d += __builtin_popcount(a[i] ^ b[i]);
    return d;
}

/* Returns 1 on success (score computed), 0 if too few candidates. */
static int score_pair(FtCalcSimScore_fn scoreFn, RealCapture *A, RealCapture *B, int canvas,
                       float *outScore, int *outCand, int *outN)
{
    ST_Feature *fa = A->flat, *fb = B->flat;
    int na = A->n, nb = B->n;
    typedef struct { int ia, ib; } Corr;
    Corr *cand = malloc((size_t)na * sizeof(Corr));
    int ncand = 0, i, j, p, q;
    for (i = 0; i < na; i++) {
        int best = 257, second = 257, bestj = -1;
        for (j = 0; j < nb; j++) {
            int d = hamming(fa[i].bDescri, fb[j].bDescri);
            if (d < best) { second = best; best = d; bestj = j; }
            else if (d < second) second = d;
        }
        if (best <= 60 && (double)best <= 0.85 * (double)second) { cand[ncand].ia = i; cand[ncand].ib = bestj; ncand++; }
    }
    *outCand = ncand;
    if (ncand < 2) { free(cand); *outN = 0; return 0; }

    unsigned char *adj = calloc((size_t)ncand * (size_t)ncand, 1);
    int *degree = calloc((size_t)ncand, sizeof(int));
    for (p = 0; p < ncand; p++) {
        double apx = fa[cand[p].ia].x, apy = fa[cand[p].ia].y, bpx = fb[cand[p].ib].x, bpy = fb[cand[p].ib].y;
        for (q = p + 1; q < ncand; q++) {
            double aqx = fa[cand[q].ia].x, aqy = fa[cand[q].ia].y, bqx = fb[cand[q].ib].x, bqy = fb[cand[q].ib].y;
            double distA = hypot(apx - aqx, apy - aqy), distB = hypot(bpx - bqx, bpy - bqy);
            if (fabs(distA - distB) < 2.0) { adj[p*ncand+q]=1; adj[q*ncand+p]=1; degree[p]++; degree[q]++; }
        }
    }
    int seed = 0, bestDeg = -1;
    for (p = 0; p < ncand; p++) if (degree[p] > bestDeg) { bestDeg = degree[p]; seed = p; }
    int *order = malloc((size_t)ncand * sizeof(int)); int nOrder = 0;
    for (q = 0; q < ncand; q++) if (q != seed && adj[seed*ncand+q]) order[nOrder++] = q;
    for (p = 0; p < nOrder; p++) for (q = p+1; q < nOrder; q++) if (degree[order[q]] > degree[order[p]]) { int t=order[p]; order[p]=order[q]; order[q]=t; }
    int *idx = malloc((size_t)ncand * sizeof(int)); int n = 0;
    idx[n++] = seed;
    for (p = 0; p < nOrder; p++) {
        int cq = order[p], ok = 1;
        for (q = 0; q < n; q++) if (!adj[cq*ncand+idx[q]]) { ok = 0; break; }
        if (ok) idx[n++] = cq;
    }
    *outN = n;
    free(adj); free(degree); free(order);

    if (n < 2) { free(idx); free(cand); return 0; }

    double sumDot=0, sumCross=0, sumSumDot=0, sumSumCross=0;
    for (i = 0; i < n; i++) {
        double aix=fa[cand[idx[i]].ia].x, aiy=fa[cand[idx[i]].ia].y, bix=fb[cand[idx[i]].ib].x, biy=fb[cand[idx[i]].ib].y;
        sumDot += aix*bix + aiy*biy; sumCross += bix*aiy - aix*biy;
        for (j = 0; j < n; j++) {
            double ajx=fa[cand[idx[j]].ia].x, ajy=fa[cand[idx[j]].ia].y, bjx=fb[cand[idx[j]].ib].x, bjy=fb[cand[idx[j]].ib].y;
            sumSumDot -= bix*ajx + biy*ajy; sumSumCross += aix*bjy - aiy*bjx;
        }
    }
    double numerator = (double)n*sumDot + sumSumDot, denominator = (double)n*sumCross + sumSumCross;
    double theta = atan2(denominator, numerator);
    double c = cos(theta), s = sin(theta);
    double accX = 0, accY = 0;
    for (i = 0; i < n; i++) {
        double aix=fa[cand[idx[i]].ia].x, aiy=fa[cand[idx[i]].ia].y, bix=fb[cand[idx[i]].ib].x, biy=fb[cand[idx[i]].ib].y;
        accX += aix - (bix*c - biy*s); accY += aiy - (bix*s + biy*c);
    }
    double dx = accX/n, dy = accY/n;
    free(idx); free(cand);

    FP32 H[6] = { (FP32)c, (FP32)(-s), (FP32)dx, (FP32)s, (FP32)c, (FP32)dy };
    UINT16 overlapPair[2] = {0, 0};
    *outScore = scoreFn(A->mask, A->bin, B->mask, B->bin, canvas, canvas, H, overlapPair);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <so_path> label1:file1.raw label2:file2.raw ...\n", argv[0]);
        return 1;
    }
    const char *soPath = argv[1];
    const int canvas = 96;
    int nfiles = argc - 2;

    void *handle = dlopen(soPath, RTLD_LAZY);
    if (!handle) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    void *anchor = dlsym(handle, "fp_device_verify");
    if (!anchor) { fprintf(stderr, "dlsym failed: %s\n", dlerror()); return 1; }
    uintptr_t bias = (uintptr_t)anchor - OFF_fp_device_verify;
    FtCalcSimScore_fn scoreFn = (FtCalcSimScore_fn)(bias + OFF_FtCalcSimScore);

    RealCapture *caps = calloc((size_t)nfiles, sizeof(RealCapture));
    int i, j;
    for (i = 0; i < nfiles; i++) {
        char *arg = argv[i + 2];
        char *colon = strchr(arg, ':');
        const char *path = colon ? colon + 1 : arg;
        char label[64];
        if (colon) { size_t len = (size_t)(colon - arg); if (len > 63) len = 63; memcpy(label, arg, len); label[len] = 0; }
        else strcpy(label, arg);
        load_real_capture(bias, label, path, canvas, &caps[i]);
        fprintf(stderr, "loaded %s (%s): %d features\n", label, path, caps[i].n);
    }

    double sameSum = 0, diffSum = 0;
    int sameN = 0, diffN = 0, failN = 0;
    double sameMin = 1e9, sameMax = -1e9, diffMin = 1e9, diffMax = -1e9;

    printf("\n=== all pairwise REAL FtCalcSimScore results ===\n");
    for (i = 0; i < nfiles; i++) {
        for (j = i + 1; j < nfiles; j++) {
            float score; int cand, n;
            int ok = score_pair(scoreFn, &caps[i], &caps[j], canvas, &score, &cand, &n);
            int same = strcmp(caps[i].label, caps[j].label) == 0;
            if (!ok) {
                printf("  %s vs %s: FAILED (candidates=%d, seedSetN=%d)  [%s]\n",
                       caps[i].path, caps[j].path, cand, n, same ? "SAME" : "DIFF");
                failN++;
                continue;
            }
            printf("  %-28s vs %-28s: score=%.4f (candidates=%d, seedSetN=%d)  [%s]\n",
                   caps[i].path, caps[j].path, score, cand, n, same ? "SAME" : "DIFF");
            if (same) {
                sameSum += score; sameN++;
                if (score < sameMin) sameMin = score;
                if (score > sameMax) sameMax = score;
            } else {
                diffSum += score; diffN++;
                if (score < diffMin) diffMin = score;
                if (score > diffMax) diffMax = score;
            }
        }
    }

    printf("\n=== SUMMARY (real FtCalcSimScore, real binarization/masks, our validated alignment) ===\n");
    if (sameN) printf("  SAME-finger:      n=%d  avg=%.4f  min=%.4f  max=%.4f\n", sameN, sameSum/sameN, sameMin, sameMax);
    if (diffN) printf("  DIFFERENT-finger: n=%d  avg=%.4f  min=%.4f  max=%.4f\n", diffN, diffSum/diffN, diffMin, diffMax);
    if (failN) printf("  FAILED (too few candidates): n=%d\n", failN);
    if (sameN && diffN) {
        printf("  gap (same_avg - diff_avg) = %.4f\n", sameSum/sameN - diffSum/diffN);
        printf("  same range [%.4f, %.4f]  diff range [%.4f, %.4f]  overlap=%s\n",
               sameMin, sameMax, diffMin, diffMax,
               (sameMin <= diffMax && diffMin <= sameMax) ? "YES" : "NO");
    }
    return 0;
}
