/*
 * DIAGNOSTIC-ONLY tool. Tests the "multi-frame/multi-touch fusion" option
 * raised in research/PROTOCOL.md's viability discussion: real vendor
 * function names (FtTemplateExtraAreaRefresh, ST_FocalTemplate's
 * templateExtendArea/templateCoinHmatrix/templateCoinFlag fields,
 * FtTemplateCoinArea, gSensorInfor.enrollMaxTplCount=16) confirm the real
 * algorithm DOES accumulate/extend a template's effective area across
 * multiple enrollment touches -- but the exact incremental state-machine
 * algorithm (FtSubTemplateCopy/FtSetCoinFlg orchestration inside the giant
 * FtVerifyByTemplate) was judged too costly to fully reverse-engineer.
 *
 * This tool tests the underlying HYPOTHESIS (does combining several
 * enrollment touches into one composite reference improve discrimination)
 * using a reasonable, general-purpose fusion of our own (align N
 * "enrollment" captures into one reference frame via this project's own
 * validated rigid-fit alignment, then majority-vote-fuse their real
 * FtGenBinImg/FtSegmentByLocalVariance outputs into a single composite
 * mask+bin image covering their UNION of area), scored against test
 * captures using the REAL FtCalcSimScore -- clearly distinguishing "our
 * fusion logic" from "the real scoring formula" throughout.
 *
 * NEVER shipped, NEVER used at runtime -- diagnostic-only.
 *
 * Build: gcc -O0 -g -Wall ground_truth_fusion_test.c -o ground_truth_fusion_test -ldl -lm
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

typedef struct { SINT32 depth, width, height, imageSize, widthStep; UINT8 *imageData; } ST_IplImage;
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
    for (r = 0; r < rows; r++) memcpy(canvasBuf + (r + padRows) * canvas + padCols, tight + r * cols, (size_t)cols);
    return canvasBuf;
}
static void unpack_bits_to_bytes(const UINT64 *bits, UINT8 *out, int n)
{
    int i;
    for (i = 0; i < n; i++) { UINT64 word = bits[i/64]; out[i] = (word >> (i%64)) & 1; }
}

typedef struct {
    char path[256];
    UINT8 *canvasBuf, *mask, *bin;
    ST_Feature *flat;
    int n;
} RealCapture;

static void load_real_capture(uintptr_t bias, const char *path, int canvas, RealCapture *out)
{
    strncpy(out->path, path, sizeof(out->path)-1);
    const int rows = 80, cols = 64;
    unsigned char *tight = load_raw_be16_as_u8(path, rows*cols);
    out->canvasBuf = pad_to_canvas(tight, rows, cols, canvas);
    free(tight);
    FtSegmentByLocalVariance_fn segFn = (FtSegmentByLocalVariance_fn)(bias + OFF_FtSegmentByLocalVariance);
    out->mask = malloc((size_t)canvas*canvas);
    segFn(out->canvasBuf, canvas, canvas, 9, 12.0f, out->mask);
    FtGenBinImg_fn binFn = (FtGenBinImg_fn)(bias + OFF_FtGenBinImg);
    ST_IplImage ipl = { .depth=8, .width=canvas, .height=canvas, .imageSize=canvas*canvas, .widthStep=canvas, .imageData=out->canvasBuf };
    UINT64 *bitArr = NULL; UINT16 arrLen = 0;
    binFn(&ipl, &bitArr, &arrLen);
    out->bin = malloc((size_t)canvas*canvas);
    if (bitArr) unpack_bits_to_bytes(bitArr, out->bin, canvas*canvas); else memset(out->bin, 0, (size_t)canvas*canvas);
    FtGetMfbFeatures_fn featFn = (FtGetMfbFeatures_fn)(bias + OFF_FtGetMfbFeatures);
    unsigned char *validMask = malloc(1152); memset(validMask, 0xFF, 1152);
    unsigned char *badPixMask = malloc(1152); memset(badPixMask, 0xFF, 1152);
    ST_InputForTemplate inPara = {
        .intvls=3, .sigma=1.6f, .contrThr=0.02f, .curvThr=15, .imgDbl=1, .descrWidth=4, .descrHistBins=8,
        .img=&ipl, .octave=4, .validArea=100, .validFlg=validMask, .badPixselValidFlg=badPixMask,
        .isFT9391=0, .algType=0, .sensorCol=96, .isSpeedUp=0, .imgScale=1.5f
    };
    ST_Feature *f1=NULL, *f2=NULL;
    ST_EXTREMUM_NUM r2 = featFn(inPara, &f1, &f2);
    out->n = r2.nMaxExtremum + r2.nMinExtremum;
    out->flat = malloc((size_t)out->n * sizeof(ST_Feature));
    int k=0, i;
    for (i=0;i<r2.nMaxExtremum;i++) out->flat[k++]=f1[i];
    for (i=0;i<r2.nMinExtremum;i++) out->flat[k++]=f2[i];
    free(validMask); free(badPixMask);
}

static int hamming(const UINT32 a[8], const UINT32 b[8]) { int i,d=0; for(i=0;i<8;i++) d+=__builtin_popcount(a[i]^b[i]); return d; }

/* Computes the rigid fit theta,dx,dy such that A ~= R(theta)*B + (dx,dy).
 * Returns 0 on failure (too few correspondences). */
static int rigid_fit(RealCapture *A, RealCapture *B, double *outTheta, double *outDx, double *outDy, int *outN)
{
    ST_Feature *fa=A->flat, *fb=B->flat;
    int na=A->n, nb=B->n, i, j, p, q;
    typedef struct { int ia, ib; } Corr;
    Corr *cand = malloc((size_t)na*sizeof(Corr));
    int ncand=0;
    for (i=0;i<na;i++) {
        int best=257, second=257, bestj=-1;
        for (j=0;j<nb;j++) { int d=hamming(fa[i].bDescri, fb[j].bDescri); if (d<best){second=best;best=d;bestj=j;} else if (d<second) second=d; }
        if (best<=60 && (double)best<=0.85*(double)second) { cand[ncand].ia=i; cand[ncand].ib=bestj; ncand++; }
    }
    if (ncand<2) { free(cand); return 0; }
    unsigned char *adj = calloc((size_t)ncand*(size_t)ncand,1);
    int *degree = calloc((size_t)ncand, sizeof(int));
    for (p=0;p<ncand;p++) {
        double apx=fa[cand[p].ia].x, apy=fa[cand[p].ia].y, bpx=fb[cand[p].ib].x, bpy=fb[cand[p].ib].y;
        for (q=p+1;q<ncand;q++) {
            double aqx=fa[cand[q].ia].x, aqy=fa[cand[q].ia].y, bqx=fb[cand[q].ib].x, bqy=fb[cand[q].ib].y;
            double dA=hypot(apx-aqx,apy-aqy), dB=hypot(bpx-bqx,bpy-bqy);
            if (fabs(dA-dB)<2.0) { adj[p*ncand+q]=1; adj[q*ncand+p]=1; degree[p]++; degree[q]++; }
        }
    }
    int seed=0,bestDeg=-1;
    for (p=0;p<ncand;p++) if (degree[p]>bestDeg) { bestDeg=degree[p]; seed=p; }
    int *order=malloc((size_t)ncand*sizeof(int)); int nOrder=0;
    for (q=0;q<ncand;q++) if (q!=seed && adj[seed*ncand+q]) order[nOrder++]=q;
    for (p=0;p<nOrder;p++) for (q=p+1;q<nOrder;q++) if (degree[order[q]]>degree[order[p]]) { int t=order[p]; order[p]=order[q]; order[q]=t; }
    int *idx=malloc((size_t)ncand*sizeof(int)); int n=0;
    idx[n++]=seed;
    for (p=0;p<nOrder;p++) { int cq=order[p], ok=1; for (q=0;q<n;q++) if(!adj[cq*ncand+idx[q]]){ok=0;break;} if(ok) idx[n++]=cq; }
    free(adj); free(degree); free(order);
    if (n<2) { free(idx); free(cand); return 0; }
    double sumDot=0,sumCross=0,sumSumDot=0,sumSumCross=0;
    for (i=0;i<n;i++) {
        double aix=fa[cand[idx[i]].ia].x, aiy=fa[cand[idx[i]].ia].y, bix=fb[cand[idx[i]].ib].x, biy=fb[cand[idx[i]].ib].y;
        sumDot+=aix*bix+aiy*biy; sumCross+=bix*aiy-aix*biy;
        for (j=0;j<n;j++) {
            double ajx=fa[cand[idx[j]].ia].x, ajy=fa[cand[idx[j]].ia].y, bjx=fb[cand[idx[j]].ib].x, bjy=fb[cand[idx[j]].ib].y;
            sumSumDot-=bix*ajx+biy*ajy; sumSumCross+=aix*bjy-aiy*bjx;
        }
    }
    double numerator=(double)n*sumDot+sumSumDot, denominator=(double)n*sumCross+sumSumCross;
    double theta=atan2(denominator, numerator);
    double c=cos(theta), s=sin(theta), accX=0, accY=0;
    for (i=0;i<n;i++) {
        double aix=fa[cand[idx[i]].ia].x, aiy=fa[cand[idx[i]].ia].y, bix=fb[cand[idx[i]].ib].x, biy=fb[cand[idx[i]].ib].y;
        accX += aix-(bix*c-biy*s); accY += aiy-(bix*s+biy*c);
    }
    *outTheta=theta; *outDx=accX/n; *outDy=accY/n; *outN=n;
    free(idx); free(cand);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <so_path> <ref.raw> <enroll2.raw> [enroll3.raw ...] -- <test1.raw label1> [test2.raw label2 ...]\n", argv[0]);
        fprintf(stderr, "  (labels: 'SAME' or 'DIFF', paired with each test file)\n");
        return 1;
    }
    const char *soPath = argv[1];
    const int canvas = 96, n = canvas*canvas;

    void *handle = dlopen(soPath, RTLD_LAZY);
    if (!handle) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    void *anchor = dlsym(handle, "fp_device_verify");
    uintptr_t bias = (uintptr_t)anchor - OFF_fp_device_verify;
    FtCalcSimScore_fn scoreFn = (FtCalcSimScore_fn)(bias + OFF_FtCalcSimScore);

    /* Parse: ref + enrollment files up to "--", then test file/label pairs after. */
    int sep = -1, i;
    for (i = 2; i < argc; i++) if (strcmp(argv[i], "--") == 0) { sep = i; break; }
    if (sep < 0) { fprintf(stderr, "missing -- separator\n"); return 1; }
    int nEnroll = sep - 2;
    int nTest = (argc - sep - 1) / 2;

    RealCapture *enroll = calloc((size_t)nEnroll, sizeof(RealCapture));
    for (i = 0; i < nEnroll; i++) { load_real_capture(bias, argv[2+i], canvas, &enroll[i]); fprintf(stderr, "enrollment[%d]=%s features=%d\n", i, argv[2+i], enroll[i].n); }

    /* Fuse enroll[1..] into enroll[0]'s reference frame via majority vote. */
    int *voteSum = calloc((size_t)n, sizeof(int));
    int *voteCnt = calloc((size_t)n, sizeof(int));
    /* enroll[0] contributes directly (identity transform). */
    for (i = 0; i < n; i++) if (enroll[0].mask[i]) { voteSum[i] += enroll[0].bin[i]; voteCnt[i]++; }
    for (i = 1; i < nEnroll; i++) {
        double theta, dx, dy; int fitN;
        if (!rigid_fit(&enroll[0], &enroll[i], &theta, &dx, &dy, &fitN)) {
            fprintf(stderr, "  fusion: enroll[%d] failed to align to reference, skipping\n", i);
            continue;
        }
        fprintf(stderr, "  fusion: enroll[%d] aligned to ref: theta=%.2fdeg dx=%.2f dy=%.2f (n=%d)\n", i, theta*180.0/M_PI, dx, dy, fitN);
        double c = cos(theta), s = sin(theta);
        int r, cc;
        for (r = 0; r < canvas; r++) {
            for (cc = 0; cc < canvas; cc++) {
                /* inverse warp: given ref pixel (r,cc), find enroll[i]'s source pixel */
                double sx = c*(cc-dx) + s*(r-dy);
                double sy = -s*(cc-dx) + c*(r-dy);
                int ix = (int)lround(sx), iy = (int)lround(sy);
                if (ix < 0 || ix >= canvas || iy < 0 || iy >= canvas) continue;
                int srcIdx = iy*canvas+ix, dstIdx = r*canvas+cc;
                if (enroll[i].mask[srcIdx]) { voteSum[dstIdx] += enroll[i].bin[srcIdx]; voteCnt[dstIdx]++; }
            }
        }
    }
    UINT8 *fusedMask = malloc((size_t)n), *fusedBin = malloc((size_t)n);
    long fusedValid = 0, refValid = 0;
    for (i = 0; i < n; i++) {
        fusedMask[i] = voteCnt[i] > 0 ? 1 : 0;
        fusedBin[i] = (voteCnt[i] > 0 && voteSum[i]*2 >= voteCnt[i]) ? 1 : 0;
        if (fusedMask[i]) fusedValid++;
        if (enroll[0].mask[i]) refValid++;
    }
    fprintf(stderr, "\nfused template valid area: %.1f%% of canvas (vs single-reference %.1f%%) -- ratio %.2fx\n",
            100.0*fusedValid/n, 100.0*refValid/n, (double)fusedValid/refValid);

    /* Test each held-out/probe capture against BOTH the fused template and the single reference alone. */
    printf("\n=== FUSED template (%d enrollment captures) vs single-reference (%s alone) ===\n", nEnroll, argv[2]);
    for (i = 0; i < nTest; i++) {
        const char *testPath = argv[sep+1+2*i];
        const char *label = argv[sep+2+2*i];
        RealCapture test = {0};
        load_real_capture(bias, testPath, canvas, &test);

        double theta, dx, dy; int fitN;
        if (!rigid_fit(&enroll[0], &test, &theta, &dx, &dy, &fitN)) {
            printf("  %-40s [%s]: alignment FAILED (too few candidates)\n", testPath, label);
            continue;
        }
        double c = cos(theta), s = sin(theta);
        FP32 H[6] = { (FP32)c, (FP32)(-s), (FP32)dx, (FP32)s, (FP32)c, (FP32)dy };
        UINT16 overlapPair[2] = {0,0};
        float fusedScore = scoreFn(fusedMask, fusedBin, test.mask, test.bin, canvas, canvas, H, overlapPair);
        float singleScore = scoreFn(enroll[0].mask, enroll[0].bin, test.mask, test.bin, canvas, canvas, H, overlapPair);
        printf("  %-40s [%s]: fused_score=%.4f  single_ref_score=%.4f  (fit n=%d)\n",
               testPath, label, fusedScore, singleScore, fitN);
    }
    return 0;
}
