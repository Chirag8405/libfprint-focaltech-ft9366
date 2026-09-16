/*
 * Clean-room reimplementation of the FocalTech matching/scoring stage
 * (Step 4 -- FtVerifyTwoTemplate), built from this project's own
 * reverse-engineering (research/PROTOCOL.md, "MILESTONE: FtVerifyTwoTemplate
 * architecture mapped, core scoring formula concretely nailed").
 *
 * Status: the FINAL SCORE FORMULA is CONFIRMED via raw disassembly (not
 * just decompiler output): agreement rate between two binarized ridge
 * images, within their mutually-valid foreground region, after applying
 * an estimated 2D affine alignment. The alignment-estimation strategy
 * here (Hamming-distance candidate correspondences -> RANSAC minimal-
 * sample affine fit -> least-squares refit on inliers) is a standard,
 * well-documented robust-estimation approach -- NOT a byte-exact
 * replication of FocalTech's own RANSAC heuristics (FtRansacAngle_32f/
 * FtRansacEdage_32f), which were explicitly not traced in depth (a
 * deliberate scope decision, see PROTOCOL.md). This is a first-draft,
 * UNTESTED reimplementation of the overall architecture.
 *
 * Build: gcc -O2 -Wall -c focal_verify.c -o focal_verify.o -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { float x, y, ori; unsigned int desc[8]; } FocalFeature;
extern int focal_hamming_distance(const unsigned int a[8], const unsigned int b[8]);

typedef struct { double a, b, c, d, e, f; } Affine2D; /* x'=a*x+b*y+e; y'=c*x+d*y+f */

static void affine_apply(const Affine2D *H, double x, double y, double *ox, double *oy)
{
    *ox = H->a * x + H->b * y + H->e;
    *oy = H->c * x + H->d * y + H->f;
}

/* Exact affine fit from exactly 3 non-collinear point correspondences
 * (the RANSAC minimal sample). Solves the two independent 3x3 linear
 * systems [x y 1][a;b;e]=x' and [x y 1][c;d;f]=y' directly. */
static int affine_from_3pts(const double sx[3], const double sy[3],
                             const double dx[3], const double dy[3], Affine2D *H)
{
    double M[3][3] = {
        { sx[0], sy[0], 1 }, { sx[1], sy[1], 1 }, { sx[2], sy[2], 1 }
    };
    double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
               - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
               + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    if (fabs(det) < 1e-9) return 0;
    double inv[3][3];
    double id = 1.0 / det;
    inv[0][0] = (M[1][1]*M[2][2]-M[1][2]*M[2][1])*id;
    inv[0][1] = (M[0][2]*M[2][1]-M[0][1]*M[2][2])*id;
    inv[0][2] = (M[0][1]*M[1][2]-M[0][2]*M[1][1])*id;
    inv[1][0] = (M[1][2]*M[2][0]-M[1][0]*M[2][2])*id;
    inv[1][1] = (M[0][0]*M[2][2]-M[0][2]*M[2][0])*id;
    inv[1][2] = (M[0][2]*M[1][0]-M[0][0]*M[1][2])*id;
    inv[2][0] = (M[1][0]*M[2][1]-M[1][1]*M[2][0])*id;
    inv[2][1] = (M[0][1]*M[2][0]-M[0][0]*M[2][1])*id;
    inv[2][2] = (M[0][0]*M[1][1]-M[0][1]*M[1][0])*id;

    H->a = inv[0][0]*dx[0] + inv[0][1]*dx[1] + inv[0][2]*dx[2];
    H->b = inv[1][0]*dx[0] + inv[1][1]*dx[1] + inv[1][2]*dx[2];
    H->e = inv[2][0]*dx[0] + inv[2][1]*dx[1] + inv[2][2]*dx[2];
    H->c = inv[0][0]*dy[0] + inv[0][1]*dy[1] + inv[0][2]*dy[2];
    H->d = inv[1][0]*dy[0] + inv[1][1]*dy[1] + inv[1][2]*dy[2];
    H->f = inv[2][0]*dy[0] + inv[2][1]*dy[1] + inv[2][2]*dy[2];
    return 1;
}

/* CONFIRMED structural match: OpenCV-style/FtGetAffineTrans_32f least-
 * squares affine fit from N>=3 correspondences (normal equations). */
static int affine_least_squares(const double *sx, const double *sy,
                                 const double *dx, const double *dy, int n, Affine2D *H)
{
    double Sxx=0,Sxy=0,Syy=0,Sx=0,Sy=0,Sxdx=0,Sydx=0,Sdx=0,Sxdy=0,Sydy=0,Sdy=0;
    int i;
    for (i = 0; i < n; i++) {
        Sxx += sx[i]*sx[i]; Sxy += sx[i]*sy[i]; Syy += sy[i]*sy[i];
        Sx += sx[i]; Sy += sy[i];
        Sxdx += sx[i]*dx[i]; Sydx += sy[i]*dx[i]; Sdx += dx[i];
        Sxdy += sx[i]*dy[i]; Sydy += sy[i]*dy[i]; Sdy += dy[i];
    }
    double A[3][3] = { {Sxx,Sxy,Sx}, {Sxy,Syy,Sy}, {Sx,Sy,(double)n} };
    double det = A[0][0]*(A[1][1]*A[2][2]-A[1][2]*A[2][1])
               - A[0][1]*(A[1][0]*A[2][2]-A[1][2]*A[2][0])
               + A[0][2]*(A[1][0]*A[2][1]-A[1][1]*A[2][0]);
    if (fabs(det) < 1e-9) return 0;
    double inv[3][3], id = 1.0/det;
    inv[0][0]=(A[1][1]*A[2][2]-A[1][2]*A[2][1])*id;
    inv[0][1]=(A[0][2]*A[2][1]-A[0][1]*A[2][2])*id;
    inv[0][2]=(A[0][1]*A[1][2]-A[0][2]*A[1][1])*id;
    inv[1][0]=(A[1][2]*A[2][0]-A[1][0]*A[2][2])*id;
    inv[1][1]=(A[0][0]*A[2][2]-A[0][2]*A[2][0])*id;
    inv[1][2]=(A[0][2]*A[1][0]-A[0][0]*A[1][2])*id;
    inv[2][0]=(A[1][0]*A[2][1]-A[1][1]*A[2][0])*id;
    inv[2][1]=(A[0][1]*A[2][0]-A[0][0]*A[2][1])*id;
    inv[2][2]=(A[0][0]*A[1][1]-A[0][1]*A[1][0])*id;

    H->a = inv[0][0]*Sxdx + inv[0][1]*Sydx + inv[0][2]*Sdx;
    H->b = inv[1][0]*Sxdx + inv[1][1]*Sydx + inv[1][2]*Sdx;
    H->e = inv[2][0]*Sxdx + inv[2][1]*Sydx + inv[2][2]*Sdx;
    H->c = inv[0][0]*Sxdy + inv[0][1]*Sydy + inv[0][2]*Sdy;
    H->d = inv[1][0]*Sxdy + inv[1][1]*Sydy + inv[1][2]*Sdy;
    H->f = inv[2][0]*Sxdy + inv[2][1]*Sydy + inv[2][2]*Sdy;
    return 1;
}

typedef struct { int ia, ib; } Correspondence;

/* BEST-EFFORT: candidate correspondence generation via best-Hamming-match
 * per feature (with a distance cutoff). Not FocalTech's own traced
 * FtRecallBinCheck/getLocalFeature (not disassembled in depth). */
static int find_candidates(const FocalFeature *A, int na, const FocalFeature *B, int nb,
                            int maxDist, Correspondence **out)
{
    Correspondence *c = malloc((size_t)na * sizeof(Correspondence));
    int n = 0, i, j;
    for (i = 0; i < na; i++) {
        int best = 257, bestj = -1;
        for (j = 0; j < nb; j++) {
            int d = focal_hamming_distance(A[i].desc, B[j].desc);
            if (d < best) { best = d; bestj = j; }
        }
        if (best <= maxDist) { c[n].ia = i; c[n].ib = bestj; n++; }
    }
    *out = c;
    return n;
}

/* BEST-EFFORT: standard RANSAC affine estimation (random 3-point minimal
 * sample, consensus counting, least-squares refit on inliers) -- NOT a
 * replication of FtRansacAngle_32f/FtRansacEdage_32f's own specific
 * sampling heuristic (deliberately not traced in depth, PROTOCOL.md). */
static int ransac_affine(const FocalFeature *A, const FocalFeature *B,
                          const Correspondence *cand, int ncand,
                          double inlierPx, int iters, Affine2D *bestH, int *inlierFlags)
{
    if (ncand < 3) return 0;
    int bestCount = 0;
    Affine2D bestModel = {1,0,0,1,0,0};
    int *flags = malloc((size_t)ncand * sizeof(int));
    srand(12345); /* deterministic for reproducibility across runs */

    int it;
    for (it = 0; it < iters; it++) {
        int i0 = rand() % ncand, i1 = rand() % ncand, i2 = rand() % ncand;
        if (i0 == i1 || i1 == i2 || i0 == i2) continue;
        double sx[3] = { A[cand[i0].ia].x, A[cand[i1].ia].x, A[cand[i2].ia].x };
        double sy[3] = { A[cand[i0].ia].y, A[cand[i1].ia].y, A[cand[i2].ia].y };
        double dx[3] = { B[cand[i0].ib].x, B[cand[i1].ib].x, B[cand[i2].ib].x };
        double dy[3] = { B[cand[i0].ib].y, B[cand[i1].ib].y, B[cand[i2].ib].y };
        Affine2D H;
        if (!affine_from_3pts(sx, sy, dx, dy, &H)) continue;

        int count = 0, k;
        for (k = 0; k < ncand; k++) {
            double ox, oy;
            affine_apply(&H, A[cand[k].ia].x, A[cand[k].ia].y, &ox, &oy);
            double dxp = ox - B[cand[k].ib].x, dyp = oy - B[cand[k].ib].y;
            flags[k] = (dxp*dxp + dyp*dyp <= inlierPx*inlierPx) ? 1 : 0;
            count += flags[k];
        }
        if (count > bestCount) {
            bestCount = count;
            bestModel = H;
            memcpy(inlierFlags, flags, (size_t)ncand * sizeof(int));
        }
    }
    free(flags);
    if (bestCount < 3) return 0;

    /* Refit on all inliers (matches FtGetAffineTrans_32f's role: a
     * least-squares fit, here run on the RANSAC consensus set). */
    double *sx = malloc((size_t)bestCount * sizeof(double));
    double *sy = malloc((size_t)bestCount * sizeof(double));
    double *dx = malloc((size_t)bestCount * sizeof(double));
    double *dy = malloc((size_t)bestCount * sizeof(double));
    int n = 0, k;
    for (k = 0; k < ncand; k++)
        if (inlierFlags[k]) {
            sx[n] = A[cand[k].ia].x; sy[n] = A[cand[k].ia].y;
            dx[n] = B[cand[k].ib].x; dy[n] = B[cand[k].ib].y;
            n++;
        }
    Affine2D refit;
    int ok = affine_least_squares(sx, sy, dx, dy, n, &refit);
    *bestH = ok ? refit : bestModel;
    free(sx); free(sy); free(dx); free(dy);
    return bestCount;
}

/* CONFIRMED via raw disassembly (research/PROTOCOL.md, "FtCalcSimScore
 * ... FULLY TRACED via raw disassembly"): agreement-rate score between
 * two binarized+masked images after applying an affine alignment. */
static float calc_sim_score(const unsigned char *maskA, const unsigned char *binA,
                             const unsigned char *maskB, const unsigned char *binB,
                             int rows, int cols, const Affine2D *H, int *overlapOut)
{
    int cnt[4] = {0,0,0,0};
    int validCnt = 0, overlap = 0;
    int r, c;
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            double ox, oy;
            affine_apply(H, c, r, &ox, &oy);
            int ix = (int)lround(ox), iy = (int)lround(oy);
            if (ix < 0 || ix >= cols || iy < 0 || iy >= rows) continue;
            overlap++;
            if (!maskA[r * cols + c] || !maskB[iy * cols + ix]) continue;
            validCnt++;
            int va = binA[r * cols + c] ? 1 : 0;
            int vb = binB[iy * cols + ix] ? 1 : 0;
            cnt[va + vb * 2]++;
        }
    }
    *overlapOut = overlap;
    if (validCnt == 0) return 0.0f;
    return (float)(cnt[0] + cnt[3]) / (float)validCnt;
}

/* Simple local-mean adaptive binarization stand-in for
 * FtGenBinImgForSamllSensor (median filter + adaptive threshold,
 * BEST-EFFORT -- see research/PROTOCOL.md for what's uncertain there). */
static void binarize_local_mean(const unsigned char *img, int rows, int cols,
                                 int blockSize, unsigned char *out)
{
    int rad = blockSize / 2;
    int r, c;
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++) {
            long sum = 0; int cnt = 0, rr, cc;
            for (rr = r - rad; rr <= r + rad; rr++) {
                if (rr < 0 || rr >= rows) continue;
                for (cc = c - rad; cc <= c + rad; cc++) {
                    if (cc < 0 || cc >= cols) continue;
                    sum += img[rr * cols + cc];
                    cnt++;
                }
            }
            double mean = (double)sum / cnt;
            out[r * cols + c] = (img[r * cols + c] > mean) ? 1 : 0;
        }
}

/* Top-level entry mirroring FtVerifyTwoTemplate's role: given two
 * feature sets + their source images (for binarization/masking), returns
 * the final similarity score (0..1) via the confirmed formula. */
float focal_verify_two_templates(const FocalFeature *A, int na, const unsigned char *imgA,
                                  const FocalFeature *B, int nb, const unsigned char *imgB,
                                  int rows, int cols, int *outInliers, int *outCandidates)
{
    Correspondence *cand;
    int ncand = find_candidates(A, na, B, nb, 90, &cand);
    *outCandidates = ncand;

    if (ncand < 3) { free(cand); *outInliers = 0; return 0.0f; }

    Affine2D H;
    int *inlierFlags = malloc((size_t)ncand * sizeof(int));
    int inliers = ransac_affine(A, B, cand, ncand, 6.0, 2000, &H, inlierFlags);
    *outInliers = inliers;
    free(inlierFlags);
    free(cand);
    if (inliers < 3) return 0.0f;

    int n = rows * cols;
    unsigned char *maskA = malloc((size_t)n), *maskB = malloc((size_t)n);
    unsigned char *binA = malloc((size_t)n), *binB = malloc((size_t)n);
    /* BEST-EFFORT stand-ins: real segmentation (focal_segment_by_local_
     * variance) and binarization not wired in here yet -- using a plain
     * local-mean threshold for both mask and bin as a first pass. */
    binarize_local_mean(imgA, rows, cols, 9, maskA);
    binarize_local_mean(imgA, rows, cols, 5, binA);
    binarize_local_mean(imgB, rows, cols, 9, maskB);
    binarize_local_mean(imgB, rows, cols, 5, binB);

    int overlap;
    float score = calc_sim_score(maskA, binA, maskB, binB, rows, cols, &H, &overlap);
    free(maskA); free(maskB); free(binA); free(binB);
    return score;
}
