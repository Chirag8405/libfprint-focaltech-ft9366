/*
 * DIAGNOSTIC-ONLY tool. Extends the established dlopen+bias technique to
 * extract REAL ground truth for FtCalcSimScore's own inputs (binarized
 * images via FtGenBinImg, validity masks via FtSegmentByLocalVariance) and
 * OUTPUT (the real score itself), for real same-finger and different-
 * finger capture pairs, using a REAL alignment derived from REAL
 * FtGetMfbFeatures keypoints. This directly tests whether the persistent
 * lack of same/different-finger separation is inherent to the real
 * scoring mechanism given real inputs, or specific to this reimplementation's
 * own approximations -- research/PROTOCOL.md ("Recommended next step").
 *
 * NEVER shipped, NEVER used at runtime -- diagnostic-only.
 *
 * Build: gcc -O0 -g -Wall ground_truth_calcsimscore.c -o ground_truth_calcsimscore -ldl -lm
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

/* Unpack FtGenBinImg's bit-packed UINT64[] output into a plain 0/1-per-
 * pixel UINT8[] array, matching the byte-per-pixel convention this
 * project's own calc_sim_score (and the real stBinlimage, per PROTOCOL.md
 * "MAJOR GROUND-TRUTH FINDING") both use. */
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
    UINT8 *canvasBuf;
    UINT8 *mask;
    UINT8 *bin;
    ST_Feature *feat1, *feat2;
    int nMax, nMin;
} RealCapture;

static void load_real_capture(void *handle, uintptr_t bias, const char *path, int canvas, RealCapture *out)
{
    const int rows = 80, cols = 64;
    unsigned char *tight = load_raw_be16_as_u8(path, rows * cols);
    out->canvasBuf = pad_to_canvas(tight, rows, cols, canvas);
    free(tight);

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
    UINT16 ret = binFn(&iplImage, &bitArr, &arrLen);
    fprintf(stderr, "FtGenBinImg(%s) returned %u, arrLen(words)=%u\n", path, ret, arrLen);
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
    ST_EXTREMUM_NUM ret2 = featFn(inPara, &out->feat1, &out->feat2);
    out->nMax = ret2.nMaxExtremum;
    out->nMin = ret2.nMinExtremum;
    free(validMask); free(badPixMask);
}

/* Combine feat1(max)+feat2(min) into one flat array for matching. */
static int flatten_features(RealCapture *c, ST_Feature **out)
{
    int n = c->nMax + c->nMin, k = 0, i;
    ST_Feature *f = malloc((size_t)n * sizeof(ST_Feature));
    for (i = 0; i < c->nMax; i++) f[k++] = c->feat1[i];
    for (i = 0; i < c->nMin; i++) f[k++] = c->feat2[i];
    *out = f;
    return n;
}
static int hamming(const UINT32 a[8], const UINT32 b[8])
{
    int i, d = 0;
    for (i = 0; i < 8; i++) d += __builtin_popcount(a[i] ^ b[i]);
    return d;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <libfprint-2.so path> <captureA.raw> <captureB.raw>\n", argv[0]);
        return 1;
    }
    const char *soPath = argv[1];
    const int canvas = 96;

    void *handle = dlopen(soPath, RTLD_LAZY);
    if (!handle) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    void *anchor = dlsym(handle, "fp_device_verify");
    if (!anchor) { fprintf(stderr, "dlsym failed: %s\n", dlerror()); return 1; }
    uintptr_t bias = (uintptr_t)anchor - OFF_fp_device_verify;

    RealCapture A = {0}, B = {0};
    load_real_capture(handle, bias, argv[2], canvas, &A);
    load_real_capture(handle, bias, argv[3], canvas, &B);

    long onesA = 0, onesB = 0, validA = 0, validB = 0;
    for (int i = 0; i < canvas*canvas; i++) { onesA += A.bin[i]; onesB += B.bin[i]; validA += A.mask[i]?1:0; validB += B.mask[i]?1:0; }
    fprintf(stderr, "A: bin ones=%.1f%% valid=%.1f%%   B: bin ones=%.1f%% valid=%.1f%%\n",
            100.0*onesA/(canvas*canvas), 100.0*validA/(canvas*canvas),
            100.0*onesB/(canvas*canvas), 100.0*validB/(canvas*canvas));

    ST_Feature *fa, *fb;
    int na = flatten_features(&A, &fa);
    int nb = flatten_features(&B, &fb);
    fprintf(stderr, "A features=%d  B features=%d\n", na, nb);

    /* Find best-Hamming candidate correspondences (ratio test), then use
     * OUR OWN confirmed-correct distance-consistency + closed-form rigid
     * fit to estimate the alignment -- reusing the same real feature data
     * both algorithms would see, so any downstream difference is isolated
     * to the SCORE computation, not the alignment. */
    typedef struct { int ia, ib; } Corr;
    Corr *cand = malloc((size_t)na * sizeof(Corr));
    int ncand = 0;
    for (int i = 0; i < na; i++) {
        int best = 257, second = 257, bestj = -1;
        for (int j = 0; j < nb; j++) {
            int d = hamming(fa[i].bDescri, fb[j].bDescri);
            if (d < best) { second = best; best = d; bestj = j; }
            else if (d < second) second = d;
        }
        if (best <= 60 && (double)best <= 0.85 * (double)second) { cand[ncand].ia = i; cand[ncand].ib = bestj; ncand++; }
    }
    fprintf(stderr, "candidates=%d\n", ncand);
    if (ncand < 2) { fprintf(stderr, "too few candidates, aborting\n"); return 1; }

    /* Distance-consistency graph + max-degree seed + greedy clique growth
     * (matching focal_verify.c's confirmed-correct rigid_ransac_angle). */
    unsigned char *adj = calloc((size_t)ncand*ncand, 1);
    int *degree = calloc((size_t)ncand, sizeof(int));
    for (int p = 0; p < ncand; p++) {
        double apx=fa[cand[p].ia].x, apy=fa[cand[p].ia].y, bpx=fb[cand[p].ib].x, bpy=fb[cand[p].ib].y;
        for (int q = p+1; q < ncand; q++) {
            double aqx=fa[cand[q].ia].x, aqy=fa[cand[q].ia].y, bqx=fb[cand[q].ib].x, bqy=fb[cand[q].ib].y;
            double distA = hypot(apx-aqx, apy-aqy), distB = hypot(bpx-bqx, bpy-bqy);
            if (fabs(distA-distB) < 2.0) { adj[p*ncand+q]=1; adj[q*ncand+p]=1; degree[p]++; degree[q]++; }
        }
    }
    int seed=0, bestDeg=-1;
    for (int p=0;p<ncand;p++) if (degree[p]>bestDeg) { bestDeg=degree[p]; seed=p; }
    int *order = malloc((size_t)ncand*sizeof(int)); int nOrder=0;
    for (int q=0;q<ncand;q++) if (q!=seed && adj[seed*ncand+q]) order[nOrder++]=q;
    for (int p=0;p<nOrder;p++) for (int q=p+1;q<nOrder;q++) if (degree[order[q]]>degree[order[p]]) { int t=order[p]; order[p]=order[q]; order[q]=t; }
    int *idx = malloc((size_t)ncand*sizeof(int)); int n=0;
    idx[n++] = seed;
    for (int p=0;p<nOrder;p++) { int cq=order[p]; int ok=1; for (int q=0;q<n;q++) if (!adj[cq*ncand+idx[q]]) { ok=0; break; } if (ok) idx[n++]=cq; }
    fprintf(stderr, "seed=%d bestDeg=%d seedSetN=%d\n", seed, bestDeg, n);

    double sumDot=0,sumCross=0,sumSumDot=0,sumSumCross=0;
    for (int i=0;i<n;i++) {
        double aix=fa[cand[idx[i]].ia].x, aiy=fa[cand[idx[i]].ia].y, bix=fb[cand[idx[i]].ib].x, biy=fb[cand[idx[i]].ib].y;
        sumDot += aix*bix+aiy*biy; sumCross += bix*aiy-aix*biy;
        for (int j=0;j<n;j++) {
            double ajx=fa[cand[idx[j]].ia].x, ajy=fa[cand[idx[j]].ia].y, bjx=fb[cand[idx[j]].ib].x, bjy=fb[cand[idx[j]].ib].y;
            sumSumDot -= bix*ajx+biy*ajy; sumSumCross += aix*bjy-aiy*bjx;
        }
    }
    double numerator = n*sumDot+sumSumDot, denominator = n*sumCross+sumSumCross;
    double theta = atan2(denominator, numerator);
    double c = cos(theta), s = sin(theta);
    double accX=0, accY=0;
    for (int i=0;i<n;i++) {
        double aix=fa[cand[idx[i]].ia].x, aiy=fa[cand[idx[i]].ia].y, bix=fb[cand[idx[i]].ib].x, biy=fb[cand[idx[i]].ib].y;
        accX += aix-(bix*c-biy*s); accY += aiy-(bix*s+biy*c);
    }
    double dx=accX/n, dy=accY/n;
    fprintf(stderr, "rigid fit: theta=%.2fdeg dx=%.2f dy=%.2f (n=%d)\n", theta*180.0/M_PI, dx, dy, n);

    /* CONFIRMED via fresh raw disassembly of FtCalcSimScore (this session,
     * re-verified specifically because it was never pinned down precisely
     * before): the real function walks the SECOND (s) image's grid
     * (row,col from the loop counters), maps each point through H into the
     * FIRST (t) image's coordinate space, and reads sMask/sBin at the raw
     * loop coordinates but tMask/tBin at the MAPPED coordinates. This is
     * the OPPOSITE walk direction from what this project's own
     * calc_sim_score assumed (which walks A's grid into B's) -- so H here
     * must be the B->A transform DIRECTLY (exactly estimate_rot_parms's
     * natural, un-inverted output: A ~= R(theta)*B + t), not the A->B
     * inverse this project computes for its own calc_sim_score. The array
     * layout is also different from this project's assumed [a,b,c,d,e,f]:
     * disassembly shows mappedCol = H[0]*col + H[1]*row + H[2],
     * mappedRow  = H[3]*col + H[4]*row + H[5] -- i.e. real layout is
     * [a,b,e,c,d,f] (e and c swapped relative to this project's own
     * in-memory Affine2D order). */
    FP32 H[6] = { (FP32)c, (FP32)(-s), (FP32)dx, (FP32)s, (FP32)c, (FP32)dy };

    FtCalcSimScore_fn scoreFn = (FtCalcSimScore_fn)(bias + OFF_FtCalcSimScore);
    /* CONFIRMED via disassembly: the "overlapSize" out-param is actually
     * TWO packed UINT16 values (validCnt at offset 0, in-bounds-count at
     * offset 2), not one -- a single UINT16 here would suffer a 2-byte
     * stack overflow when the real function writes the second field. */
    UINT16 overlapPair[2] = {0, 0};
    /* Per the confirmed walk direction: arg1/2 (tMask/tBin) = A (the
     * "destination" H maps INTO), arg3/4 (sMask/sBin) = B (the grid H
     * walks OVER), matching H's B->A direction above. */
    float realScore = scoreFn(A.mask, A.bin, B.mask, B.bin, canvas, canvas, H, overlapPair);
    printf("REAL FtCalcSimScore(%s, %s) = %.4f  (validCnt=%u, overlap=%u)\n",
           argv[2], argv[3], realScore, overlapPair[0], overlapPair[1]);

    return 0;
}
