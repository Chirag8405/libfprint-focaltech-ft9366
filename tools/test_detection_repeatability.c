/* Detection-only repeatability diagnostic (independent of the matching
 * stage entirely -- no descriptor Hamming matching, no RANSAC, no
 * FtCalcSimScore). Tests hypothesis (3) from research/PROTOCOL.md: is this
 * reimplementation's own keypoint detection (FtGetMfsFeatures equivalent)
 * repeatable across independently captured real images of the same finger,
 * or does it produce a substantially different keypoint SET each touch
 * (in which case no matching-stage fix could ever separate same/different
 * finger, since the input keypoints themselves don't correspond)?
 *
 * Method: for each pair of captures, find the best-fitting rigid transform
 * (rotation+translation) using ONLY keypoint (x,y) positions -- a brute
 * force coarse-to-fine grid search over (dx,dy,theta) that maximizes how
 * many of capture A's keypoints land within a fixed pixel radius of some
 * keypoint in capture B. This is deliberately independent of the
 * descriptor/RANSAC code already under suspicion, and independent of this
 * project's own possibly-buggy alignment estimator, so it can't inherit
 * a matching-stage bug -- it directly answers "do these two independently
 * detected keypoint sets even correspond, geometrically, at their best
 * possible alignment."
 *
 * Reusable: rerun this after any future detection-stage change to check
 * whether repeatability improved.
 *
 * Build: gcc -O2 -Wall test_detection_repeatability.c focal_sift.c -o test_detection_repeatability -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { float x, y, ori; unsigned int desc[8]; } FocalFeature;
extern int focal_extract_features(const unsigned char *img, int rows, int cols,
                                   int octaves, FocalFeature **out_features);

static int cmp_u16(const void *a, const void *b)
{
    return (int)(*(const unsigned short *)a) - (int)(*(const unsigned short *)b);
}

static unsigned char *load_raw_be16_as_u8(const char *path, int npix)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    unsigned char *buf = malloc((size_t)npix * 2);
    if (fread(buf, 1, (size_t)npix * 2, f) != (size_t)npix * 2) { fprintf(stderr, "short read %s\n", path); exit(1); }
    fclose(f);

    unsigned short *px = malloc((size_t)npix * sizeof(unsigned short));
    int i;
    for (i = 0; i < npix; i++) px[i] = (unsigned short)((buf[i * 2] << 8) | buf[i * 2 + 1]);
    free(buf);

    unsigned short *sorted = malloc((size_t)npix * sizeof(unsigned short));
    memcpy(sorted, px, (size_t)npix * sizeof(unsigned short));
    qsort(sorted, npix, sizeof(unsigned short), cmp_u16);
    unsigned short lo = sorted[(int)(npix * 0.01)];
    unsigned short hi = sorted[(int)(npix * 0.99)];
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

/* Counts how many of A's points land within `thresh` px of some point in
 * B, after transforming B by (dx,dy,thetaDeg) about the sensor center. */
static int count_matches(const FocalFeature *A, int na, const FocalFeature *B, int nb,
                          double dx, double dy, double thetaDeg, double thresh,
                          double cx, double cy)
{
    double rad = thetaDeg * M_PI / 180.0;
    double c = cos(rad), s = sin(rad);
    double bx[512], by[512];
    int j;
    for (j = 0; j < nb; j++) {
        double px = B[j].x - cx, py = B[j].y - cy;
        bx[j] = px * c - py * s + cx + dx;
        by[j] = px * s + py * c + cy + dy;
    }
    int count = 0, i;
    double thresh2 = thresh * thresh;
    for (i = 0; i < na; i++) {
        for (j = 0; j < nb; j++) {
            double ddx = A[i].x - bx[j], ddy = A[i].y - by[j];
            if (ddx * ddx + ddy * ddy <= thresh2) { count++; break; }
        }
    }
    return count;
}

/* Coarse-to-fine brute-force search over (dx,dy,theta) maximizing the
 * count of A-points with a nearby B-point (after transforming B). Returns
 * the best count found and the winning transform. */
static int best_alignment(const FocalFeature *A, int na, const FocalFeature *B, int nb,
                           double thresh, double cx, double cy,
                           double *outDx, double *outDy, double *outTheta)
{
    double bestDx = 0, bestDy = 0, bestTheta = 0;
    int bestCount = -1;
    double dx, dy, th;

    /* Coarse pass: +-25px translation (step 2), +-20deg rotation (step 2). */
    for (dx = -25; dx <= 25; dx += 2)
        for (dy = -25; dy <= 25; dy += 2)
            for (th = -20; th <= 20; th += 2) {
                int c = count_matches(A, na, B, nb, dx, dy, th, thresh, cx, cy);
                if (c > bestCount) { bestCount = c; bestDx = dx; bestDy = dy; bestTheta = th; }
            }

    /* Fine pass: refine around the coarse optimum. */
    double cdx = bestDx, cdy = bestDy, cth = bestTheta;
    for (dx = cdx - 2.5; dx <= cdx + 2.5; dx += 0.5)
        for (dy = cdy - 2.5; dy <= cdy + 2.5; dy += 0.5)
            for (th = cth - 2.5; th <= cth + 2.5; th += 0.25) {
                int c = count_matches(A, na, B, nb, dx, dy, th, thresh, cx, cy);
                if (c > bestCount) { bestCount = c; bestDx = dx; bestDy = dy; bestTheta = th; }
            }

    *outDx = bestDx; *outDy = bestDy; *outTheta = bestTheta;
    return bestCount;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s [--thresh=Npx] label1:file1.raw label2:file2.raw [more...]\n", argv[0]);
        fprintf(stderr, "  label prefix (before ':') groups files as the same physical finger,\n");
        fprintf(stderr, "  e.g. 'sameFingerA:same1.raw' 'sameFingerA:same2.raw' 'sameFingerB:diff1.raw'\n");
        return 1;
    }
    const int rows = 80, cols = 64, octaves = 4, n = rows * cols;
    double THRESH = 3.0;
    int argStart = 1;
    if (strncmp(argv[1], "--thresh=", 9) == 0) { THRESH = atof(argv[1] + 9); argStart = 2; }
    int nfiles = argc - argStart;
    argv += (argStart - 1);
    char labels[64][64];
    FocalFeature *feats[64];
    int counts[64];

    int i;
    for (i = 0; i < nfiles; i++) {
        char *arg = argv[i + 1];
        char *colon = strchr(arg, ':');
        const char *path = colon ? colon + 1 : arg;
        if (colon) { size_t len = colon - arg; memcpy(labels[i], arg, len); labels[i][len] = 0; }
        else strcpy(labels[i], arg);
        unsigned char *img = load_raw_be16_as_u8(path, n);
        counts[i] = focal_extract_features(img, rows, cols, octaves, &feats[i]);
        printf("%s (%s): %d features\n", labels[i], path, counts[i]);
        free(img);
    }

    printf("\n=== pairwise detection-only repeatability (best-alignment spatial match rate, thresh=%.1fpx) ===\n", THRESH);
    double sameSum = 0; int sameN = 0;
    double diffSum = 0; int diffN = 0;
    int j;
    for (i = 0; i < nfiles; i++) {
        for (j = i + 1; j < nfiles; j++) {
            double dx, dy, theta;
            int matched = best_alignment(feats[i], counts[i], feats[j], counts[j], THRESH,
                                          cols / 2.0, rows / 2.0, &dx, &dy, &theta);
            double rateA = 100.0 * matched / counts[i];
            double rateB = 100.0 * matched / counts[j];
            double rateAvg = 100.0 * matched / ((counts[i] + counts[j]) / 2.0);
            int same = strcmp(labels[i], labels[j]) == 0;
            printf("  %s vs %s: matched=%d/%d (A-rate=%.1f%%, B-rate=%.1f%%, avg-rate=%.1f%%) best-fit dx=%.1f dy=%.1f theta=%.1fdeg  [%s]\n",
                   labels[i], labels[j], matched, matched, rateA, rateB, rateAvg, dx, dy, theta,
                   same ? "SAME finger" : "different finger");
            if (same) { sameSum += rateAvg; sameN++; }
            else { diffSum += rateAvg; diffN++; }
        }
    }

    printf("\n=== SUMMARY ===\n");
    if (sameN) printf("  same-finger avg repeatability:      %.1f%% (n=%d pairs)\n", sameSum / sameN, sameN);
    if (diffN) printf("  different-finger avg repeatability: %.1f%% (n=%d pairs)\n", diffSum / diffN, diffN);
    if (sameN && diffN) printf("  gap (same - different):             %.1f percentage points\n", sameSum / sameN - diffSum / diffN);

    return 0;
}
