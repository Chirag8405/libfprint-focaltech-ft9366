/*
 * Clean-room reimplementation of the FocalTech matching/scoring stage
 * (Step 4 -- FtVerifyTwoTemplate), built from this project's own
 * reverse-engineering (research/PROTOCOL.md, "MILESTONE: FtVerifyTwoTemplate
 * architecture mapped, core scoring formula concretely nailed").
 *
 * Status: the FINAL SCORE FORMULA is CONFIRMED via raw disassembly (not
 * just decompiler output): agreement rate between two binarized ridge
 * images, within their mutually-valid foreground region, after applying
 * an estimated 2D rigid (rotation+translation) alignment. The alignment
 * stage (`rigid_ransac_angle`/`estimate_rot_parms`) is a direct
 * transcription of the CONFIRMED core of `FtRansacAngle_32f`/
 * `FtEstimateRotParms_32f` (raw-disassembled, research/PROTOCOL.md
 * "FtRansacAngle_32f disassembled"): candidate correspondences are
 * filtered by rotation/translation-invariant pairwise distance
 * consistency (not spatial-proximity RANSAC over a random-sample affine),
 * and the rigid transform is fit via FocalTech's own exact closed-form
 * formula. The seed-selection tie-breaking and refinement-iteration
 * bookkeeping beyond that are engineering judgment (the remaining ~700
 * lines of FtRansacAngle_32f were not traced byte-exact -- see
 * PROTOCOL.md for what's confirmed vs. best-effort here).
 *
 * Build: gcc -O2 -Wall -c focal_verify.c -o focal_verify.o -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { float x, y, ori; unsigned int desc[8]; } FocalFeature;
extern int focal_hamming_distance(const unsigned int a[8], const unsigned int b[8]);
/* CONFIRMED real segmentation primitive (focal_match.c) -- foreground/
 * background mask via local variance thresholding + erode/dilate. */
extern int focal_segment_by_local_variance(const unsigned char *src, int rows, int cols,
                                            int ksize, float thr, unsigned char *dst);
extern int focal_local_contrast_enhance(unsigned char *src, int rows, int cols, int ksize);

typedef struct { double a, b, c, d, e, f; } Affine2D; /* x'=a*x+b*y+e; y'=c*x+d*y+f */

static void affine_apply(const Affine2D *H, double x, double y, double *ox, double *oy)
{
    *ox = H->a * x + H->b * y + H->e;
    *oy = H->c * x + H->d * y + H->f;
}

typedef struct { int ia, ib; } Correspondence;

/* BEST-EFFORT: candidate correspondence generation via best-Hamming-match
 * per feature (with a distance cutoff). Not FocalTech's own traced
 * FtRecallBinCheck/getLocalFeature (not disassembled in depth). */
/* Ratio test added per the RANSAC audit conclusion (research/PROTOCOL.md,
 * "Follow-up: quantified and tested a RANSAC chance-collision hypothesis"):
 * a lone Hamming-distance cutoff is not selective enough on this sensor,
 * since candidate correspondences that are wrong on descriptor grounds can
 * still look geometrically plausible (near-identity transform) purely
 * because keypoint spatial layout is constrained by the same small
 * contact-area shape across any two captures. Requiring the best match to
 * beat the second-best by a real margin (the standard SIFT/ORB safeguard,
 * Lowe's ratio test) is intended to reject exactly this failure mode. */
static int find_candidates(const FocalFeature *A, int na, const FocalFeature *B, int nb,
                            int maxDist, double maxRatio, Correspondence **out)
{
    Correspondence *c = malloc((size_t)na * sizeof(Correspondence));
    int n = 0, i, j;
    for (i = 0; i < na; i++) {
        int best = 257, second = 257, bestj = -1;
        for (j = 0; j < nb; j++) {
            int d = focal_hamming_distance(A[i].desc, B[j].desc);
            if (d < best) { second = best; best = d; bestj = j; }
            else if (d < second) { second = d; }
        }
        if (best <= maxDist && (double)best <= maxRatio * (double)second) {
            c[n].ia = i; c[n].ib = bestj; n++;
        }
    }
    *out = c;
    return n;
}

/* CONFIRMED via raw disassembly of FtEstimateRotParms_32f (offset 0xf3f60,
 * research/PROTOCOL.md "FtRansacAngle_32f disassembled"): closed-form
 * rotation+translation (rigid, NOT full affine) least-squares fit between
 * two point sets, given a list of correspondence indices into cand[].
 * Algebraically the standard centroid-relative 2D Procrustes rotation
 * estimate `theta = atan2(Sum cross(Ai-meanA,Bi-meanB), Sum dot(Ai-meanA,
 * Bi-meanB))`, computed here via the real algorithm's own O(n^2) double-sum
 * formulation (mathematically identical, not an approximation) so this
 * stays a direct transcription rather than an independently-derived
 * equivalent. `FtArctan` itself is a custom fixed-point lookup-table
 * atan2f approximation; using libm's atan2 here is strictly more accurate,
 * not a different algorithm. */
static void estimate_rot_parms(const FocalFeature *A, const FocalFeature *B,
                                const Correspondence *cand, const int *idx, int n,
                                double *outDx, double *outDy, double *outTheta)
{
    double sumDot = 0, sumCross = 0, sumSumDot = 0, sumSumCross = 0;
    int i, j;
    for (i = 0; i < n; i++) {
        double aix = A[cand[idx[i]].ia].x, aiy = A[cand[idx[i]].ia].y;
        double bix = B[cand[idx[i]].ib].x, biy = B[cand[idx[i]].ib].y;
        sumDot += aix*bix + aiy*biy;
        sumCross += bix*aiy - aix*biy;
        for (j = 0; j < n; j++) {
            double ajx = A[cand[idx[j]].ia].x, ajy = A[cand[idx[j]].ia].y;
            double bjx = B[cand[idx[j]].ib].x, bjy = B[cand[idx[j]].ib].y;
            sumSumDot -= bix*ajx + biy*ajy;
            sumSumCross += aix*bjy - aiy*bjx;
        }
    }
    double numerator = (double)n * sumDot + sumSumDot;
    double denominator = (double)n * sumCross + sumSumCross;
    double theta = atan2(denominator, numerator);
    double c = cos(theta), s = sin(theta);
    double accX = 0, accY = 0;
    for (i = 0; i < n; i++) {
        double aix = A[cand[idx[i]].ia].x, aiy = A[cand[idx[i]].ia].y;
        double bix = B[cand[idx[i]].ib].x, biy = B[cand[idx[i]].ib].y;
        accX += aix - (bix*c - biy*s);
        accY += aiy - (bix*s + biy*c);
    }
    *outDx = accX / n; *outDy = accY / n; *outTheta = theta;
}

/* estimate_rot_parms (matching FtEstimateRotParms_32f exactly) computes the
 * rigid transform that maps B onto A: A ~= R(theta)*B + t. But calc_sim_score
 * (like the real FtCalcSimScore) walks template A's coordinate grid and maps
 * INTO template B's, i.e. needs the inverse transform B ~= R(-theta)*A - R(-theta)*t.
 * This converts the confirmed B->A (c,s,dx,dy) into the A->B Affine2D form. */
static void rigid_BtoA_to_AtoB(double c, double s, double dx, double dy, Affine2D *H)
{
    H->a = c;  H->b = s;  H->e = -(c*dx + s*dy);
    H->c = -s; H->d = c;  H->f = (s*dx - c*dy);
}

/* CONFIRMED via raw disassembly of FtRansacAngle_32f's main double loop
 * (offset 0xf6963-0xf6a51, research/PROTOCOL.md): candidate correspondences
 * are filtered by ROTATION/TRANSLATION-INVARIANT pairwise point-to-point
 * distance consistency -- |distA(i,j) - distB(i,j)| < 2.0px (exact
 * constants extracted from .rodata) -- not by spatial proximity to a
 * randomly-sampled minimal-sample transform's prediction. This is why the
 * real matcher is robust to rotation between captures where this
 * reimplementation's earlier random-3-point affine RANSAC was not (proven
 * by the synthetic rotation-sweep diagnostic, PROTOCOL.md): pairwise
 * distances within ONE template are unaffected by how much the OTHER
 * template is rotated/translated relative to it.
 *
 * Seed selection (max-degree candidate in the resulting compatibility
 * graph) is understood architecturally, not byte-exact -- the remaining
 * ~700 lines of FtRansacAngle_32f (tie-breaking, refinement-iteration
 * bookkeeping, the handoff into FtRansacEdage_32f) were not traced to the
 * last instruction, a deliberate scope decision (see PROTOCOL.md). */
static int rigid_ransac_angle(const FocalFeature *A, const FocalFeature *B,
                               const Correspondence *cand, int ncand,
                               double inlierPx, Affine2D *bestH, int *inlierFlags)
{
    if (ncand < 2) return 0;

    unsigned char *adj = calloc((size_t)ncand * (size_t)ncand, 1);
    int *degree = calloc((size_t)ncand, sizeof(int));
    int p, q;
    for (p = 0; p < ncand; p++) {
        double apx = A[cand[p].ia].x, apy = A[cand[p].ia].y;
        double bpx = B[cand[p].ib].x, bpy = B[cand[p].ib].y;
        for (q = p + 1; q < ncand; q++) {
            double aqx = A[cand[q].ia].x, aqy = A[cand[q].ia].y;
            double bqx = B[cand[q].ib].x, bqy = B[cand[q].ib].y;
            double distA = hypot(apx - aqx, apy - aqy);
            double distB = hypot(bpx - bqx, bpy - bqy);
            if (fabs(distA - distB) < 2.0) {
                adj[p * ncand + q] = 1; adj[q * ncand + p] = 1;
                degree[p]++; degree[q]++;
            }
        }
    }
    int seed = 0, bestDeg = -1;
    for (p = 0; p < ncand; p++) if (degree[p] > bestDeg) { bestDeg = degree[p]; seed = p; }

    /* Greedy clique growth: each added candidate must be pairwise-
     * consistent with EVERY current member, not just the seed. Using only
     * "consistent with the seed" (this reimplementation's first attempt)
     * is a real bug, not a simplification -- pairwise consistency is not
     * transitive, so two candidates can each match the seed's geometry
     * while being wildly inconsistent with each other (confirmed via
     * FOCAL_DEBUG_RANSAC: two candidates differing by ~50px in their own
     * pairwise check both matched the same seed and corrupted the fit).
     * Degree order (highest first) among the seed's neighbors is a
     * reasonable, standard greedy heuristic, not independently confirmed
     * against FtRansacAngle_32f's own untraced tie-breaking rule. */
    int *order = malloc((size_t)ncand * sizeof(int));
    int nOrder = 0;
    for (q = 0; q < ncand; q++) if (q != seed && adj[seed * ncand + q]) order[nOrder++] = q;
    for (p = 0; p < nOrder; p++)
        for (q = p + 1; q < nOrder; q++)
            if (degree[order[q]] > degree[order[p]]) { int t = order[p]; order[p] = order[q]; order[q] = t; }

    int *idx = malloc((size_t)ncand * sizeof(int));
    int n = 0;
    idx[n++] = seed;
    for (p = 0; p < nOrder; p++) {
        int cq = order[p];
        int ok = 1;
        for (q = 0; q < n; q++) if (!adj[cq * ncand + idx[q]]) { ok = 0; break; }
        if (ok) idx[n++] = cq;
    }
    free(order);
    free(adj); free(degree);

    if (n < 2) { free(idx); return 0; }

    if (getenv("FOCAL_DEBUG_RANSAC")) {
        for (int z = 0; z < n; z++) {
            int k2 = idx[z];
            fprintf(stderr, "  seedmember cand[%d] ia=%d ib=%d  A=(%.2f,%.2f) B=(%.2f,%.2f)\n",
                    k2, cand[k2].ia, cand[k2].ib,
                    A[cand[k2].ia].x, A[cand[k2].ia].y, B[cand[k2].ib].x, B[cand[k2].ib].y);
        }
    }
    double dx, dy, theta;
    estimate_rot_parms(A, B, cand, idx, n, &dx, &dy, &theta);
    free(idx);
    if (getenv("FOCAL_DEBUG_RANSAC"))
        fprintf(stderr, "DEBUG seed=%d bestDeg=%d seedSetN=%d theta=%.2fdeg dx=%.2f dy=%.2f\n",
                seed, bestDeg, n, theta * 180.0 / M_PI, dx, dy);

    /* Refinement pass: re-derive the inlier set from ALL candidates using
     * residuals against the fitted rigid transform (not just the seed's
     * compatibility neighborhood), then refit -- mirroring the observed
     * pattern of FtEstimateRotParms_32f being called twice. */
    double c = cos(theta), s = sin(theta);
    int count = 0, k;
    for (k = 0; k < ncand; k++) {
        double bx = B[cand[k].ib].x, by = B[cand[k].ib].y;
        double predx = bx*c - by*s + dx, predy = bx*s + by*c + dy;
        double rx = predx - A[cand[k].ia].x, ry = predy - A[cand[k].ia].y;
        inlierFlags[k] = (rx*rx + ry*ry <= inlierPx*inlierPx) ? 1 : 0;
        count += inlierFlags[k];
    }
    if (getenv("FOCAL_DEBUG_RANSAC"))
        fprintf(stderr, "DEBUG refined count=%d / ncand=%d\n", count, ncand);
    if (count < 2) {
        rigid_BtoA_to_AtoB(c, s, dx, dy, bestH);
        return count;
    }

    int *idx2 = malloc((size_t)count * sizeof(int));
    n = 0;
    for (k = 0; k < ncand; k++) if (inlierFlags[k]) idx2[n++] = k;
    double dx2, dy2, theta2;
    estimate_rot_parms(A, B, cand, idx2, n, &dx2, &dy2, &theta2);
    free(idx2);
    if (getenv("FOCAL_DEBUG_RANSAC"))
        fprintf(stderr, "DEBUG refit theta2=%.2fdeg dx2=%.2f dy2=%.2f\n", theta2*180.0/M_PI, dx2, dy2);

    rigid_BtoA_to_AtoB(cos(theta2), sin(theta2), dx2, dy2, bestH);
    return count;
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

/* CONFIRMED via raw disassembly of FtGenBinImgForSamllSensor/FtLocalThreshold
 * (research/PROTOCOL.md, this session's follow-up): the real algorithm is NOT
 * a plain "pixel > local mean" split. The exact steps, with exact extracted
 * constants:
 *   1. 3x3 median filter (FtMedianFilter, ksize=1 -- window shape/mirror
 *      padding not independently traced, using a standard 3x3 clamped
 *      median as a reasonable stand-in).
 *   2. Byte-invert the median-filtered image (FtLocalThreshold's ksize=1
 *      path does `inv[i] = ~medImg[i]`, i.e. 255-medImg[i], BEFORE any
 *      mean/var computation -- confirmed at raw disassembly offset
 *      0xffbc8 in FtLocalThreshold).
 *   3. Compute local mean and local variance of the INVERTED image over a
 *      blockSize x blockSize window (FtLocalMeanVar, blockSize=5 at the
 *      real call site).
 *   4. Per-pixel threshold (confirmed exact constants from .rodata:
 *      0.1 @0x188398, 1.0 @0x179b00, 0.0078125=1/128 @0x1a9824):
 *        threshold[i] = mean[i] * (1.0 - 0.1 + 0.1/128 * sqrt(var[i]))
 *                     = mean[i] * (0.9 + 0.00078125 * localStdDev[i])
 *      bin[i] = 0xFF if threshold[i] <= inv[i], else 0.
 * This replaces the previous placeholder (plain local-mean threshold with
 * no bias term), which is a materially different, much cruder rule. */
static void binarize_median_adaptive(const unsigned char *img, int rows, int cols,
                                      int blockSize, unsigned char *out)
{
    int n = rows * cols;
    unsigned char *med = malloc((size_t)n);
    unsigned char *inv = malloc((size_t)n);
    float *mean = malloc((size_t)n * sizeof(float));
    float *var = malloc((size_t)n * sizeof(float));
    int r, c;

    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++) {
            unsigned char win[9];
            int m = 0, rr, cc;
            for (rr = r - 1; rr <= r + 1; rr++)
                for (cc = c - 1; cc <= c + 1; cc++) {
                    int sr = rr < 0 ? 0 : (rr >= rows ? rows - 1 : rr);
                    int sc = cc < 0 ? 0 : (cc >= cols ? cols - 1 : cc);
                    win[m++] = img[sr * cols + sc];
                }
            for (int i = 1; i < m; i++) {
                unsigned char v = win[i]; int j = i - 1;
                while (j >= 0 && win[j] > v) { win[j + 1] = win[j]; j--; }
                win[j + 1] = v;
            }
            med[r * cols + c] = win[4];
        }

    for (r = 0; r < n; r++)
        inv[r] = (unsigned char)(0xFF - med[r]);

    int rad = blockSize / 2;
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++) {
            long sum = 0, sumSq = 0; int cnt = 0, rr, cc;
            for (rr = r - rad; rr <= r + rad; rr++) {
                if (rr < 0 || rr >= rows) continue;
                for (cc = c - rad; cc <= c + rad; cc++) {
                    if (cc < 0 || cc >= cols) continue;
                    int v = inv[rr * cols + cc];
                    sum += v; sumSq += v * v; cnt++;
                }
            }
            float m = (float)sum / cnt;
            mean[r * cols + c] = m;
            var[r * cols + c] = (float)sumSq / cnt - m * m;
        }

    for (r = 0; r < n; r++) {
        float v = var[r] > 0.0f ? var[r] : 0.0f;
        float threshold = mean[r] * (0.9f + 0.00078125f * sqrtf(v));
        out[r] = (threshold <= (float)inv[r]) ? 1 : 0;
    }

    free(med); free(inv); free(mean); free(var);
}

/* Top-level entry mirroring FtVerifyTwoTemplate's role: given two
 * feature sets + their source images (for binarization/masking), returns
 * the final similarity score (0..1) via the confirmed formula. */
float focal_verify_two_templates(const FocalFeature *A, int na, const unsigned char *imgA,
                                  const FocalFeature *B, int nb, const unsigned char *imgB,
                                  int rows, int cols, int *outInliers, int *outCandidates)
{
    Correspondence *cand;
    /* DIAGNOSTIC: temporary env override to sweep candidate threshold
     * empirically (2026-09-16 follow-up to the descriptor fix -- see
     * PROTOCOL.md). Defaults to the existing 90/0.85 if unset. */
    int maxDist = getenv("FOCAL_CAND_MAXDIST") ? atoi(getenv("FOCAL_CAND_MAXDIST")) : 90;
    double maxRatio = getenv("FOCAL_CAND_MAXRATIO") ? atof(getenv("FOCAL_CAND_MAXRATIO")) : 0.85;
    int ncand = find_candidates(A, na, B, nb, maxDist, maxRatio, &cand);
    *outCandidates = ncand;

    if (ncand < 3) { free(cand); *outInliers = 0; return 0.0f; }

    Affine2D H;
    int *inlierFlags = malloc((size_t)ncand * sizeof(int));
    /* Distance-consistency-graph + closed-form rigid (rotation+translation)
     * fit -- replaces the earlier random-3-point-affine RANSAC per the
     * confirmed FtRansacAngle_32f disassembly (PROTOCOL.md). The 2.5px
     * residual threshold for the refinement pass is kept from the earlier
     * chance-collision analysis (still applicable: it bounds how far a
     * wrong correspondence's predicted point may land from its actual
     * match and still count as an inlier). */
    int inliers = rigid_ransac_angle(A, B, cand, ncand, 2.5, &H, inlierFlags);
    *outInliers = inliers;
    free(inlierFlags);
    free(cand);
    if (inliers < 3) return 0.0f;

    int n = rows * cols;
    unsigned char *maskA = malloc((size_t)n), *maskB = malloc((size_t)n);
    unsigned char *binA = malloc((size_t)n), *binB = malloc((size_t)n);
    unsigned char *enhA = malloc((size_t)n), *enhB = malloc((size_t)n);
    /* Real segmentation for the validity mask (fixes the earlier crude
     * local-mean-threshold placeholder, which classified ~50% of any
     * image -- including pure background -- as "valid"). Threshold value
     * not independently confirmed from disassembly; chosen empirically
     * to give a plausible-looking foreground fraction, see PROTOCOL.md. */
    focal_segment_by_local_variance(imgA, rows, cols, 9, 12.0f, maskA);
    focal_segment_by_local_variance(imgB, rows, cols, 9, 12.0f, maskB);

    /* Real local-contrast enhancement (CONFIRMED, focal_match.c) before
     * binarizing, so ridge structure is pronounced/consistent rather than
     * thresholding the raw percentile-normalized capture directly. */
    memcpy(enhA, imgA, (size_t)n);
    memcpy(enhB, imgB, (size_t)n);
    focal_local_contrast_enhance(enhA, rows, cols, 9);
    focal_local_contrast_enhance(enhB, rows, cols, 9);
    binarize_median_adaptive(enhA, rows, cols, 5, binA);
    binarize_median_adaptive(enhB, rows, cols, 5, binB);
    free(enhA); free(enhB);

    int overlap;
    float score = calc_sim_score(maskA, binA, maskB, binB, rows, cols, &H, &overlap);
    free(maskA); free(maskB); free(binA); free(binB);
    return score;
}
