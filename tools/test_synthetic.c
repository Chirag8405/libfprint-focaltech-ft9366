/* Synthetic ground-truth diagnostic: generate provably-same-pattern "captures"
 * by applying known, controlled rigid transforms (small translation/rotation)
 * to a real preprocessed capture, and provably-different-pattern captures
 * from genuinely different real captures (also transformed). Runs the full
 * reimplemented pipeline (detection -> descriptor -> matching/scoring) on
 * these pairs, where the correct answer is known by construction, to
 * discriminate whether the persistent same/different-finger separation
 * failure lies in the matching/scoring code itself (fails even here) or is
 * specific to real capture noise/distortion the single-pass RANSAC can't
 * handle (works here, fails only on real pairs). See research/PROTOCOL.md
 * for the reasoning and how to interpret the results.
 *
 * This tool is intentionally kept generic/reusable: it is not a one-shot
 * throwaway, it is meant to be rerun after future matching-stage changes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { float x, y, ori; unsigned int desc[8]; } FocalFeature;
extern int focal_extract_features(const unsigned char *img, int rows, int cols,
                                   int octaves, FocalFeature **out_features);
extern float focal_verify_two_templates(const FocalFeature *A, int na, const unsigned char *imgA,
                                         const FocalFeature *B, int nb, const unsigned char *imgB,
                                         int rows, int cols, int *outInliers, int *outCandidates);

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

/* Applies a rigid transform (rotate about image center, then translate) to
 * produce a synthetic "different capture" of the same underlying pattern.
 * Out-of-bounds source pixels are filled with 0 (mimicking blank sensor
 * background revealed by a real physical shift), not edge-clamped. */
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
            double tx = sx - x0, ty = sy - y0;
            double v = 0.0;
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

typedef struct { double dx, dy, angle; const char *label; } Transform;

static const Transform TRANSFORMS[] = {
    { 0.0,  0.0,  0.0,  "identity" },
    { 2.0,  0.0,  0.0,  "dx=+2" },
    { -2.0, 0.0,  0.0,  "dx=-2" },
    { 0.0,  2.0,  0.0,  "dy=+2" },
    { 0.0,  -2.0, 0.0,  "dy=-2" },
    { 0.0,  0.0,  3.0,  "rot=+3deg" },
    { 0.0,  0.0,  -3.0, "rot=-3deg" },
    { 2.0,  1.0,  3.0,  "dx=+2,dy=+1,rot=+3deg" },
    { -1.0, 2.0,  -2.0, "dx=-1,dy=+2,rot=-2deg" },
};
#define NUM_TRANSFORMS (int)(sizeof(TRANSFORMS) / sizeof(TRANSFORMS[0]))

static float verify_pair(const unsigned char *imgA, int rows, int cols,
                          const unsigned char *imgB, int octaves,
                          int *outCand, int *outInliers)
{
    FocalFeature *fa, *fb;
    int na = focal_extract_features(imgA, rows, cols, octaves, &fa);
    int nb = focal_extract_features(imgB, rows, cols, octaves, &fb);
    float score = focal_verify_two_templates(fa, na, imgA, fb, nb, imgB, rows, cols, outInliers, outCand);
    free(fa); free(fb);
    return score;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s same_finger.raw different_finger.raw [more_different_fingers.raw ...]\n", argv[0]);
        return 1;
    }
    const int rows = 80, cols = 64, octaves = 4, n = rows * cols;
    unsigned char *base = load_raw_be16_as_u8(argv[1], n);

    printf("=== STEP 1: synthetic same-image test (base=%s vs transformed copies of itself) ===\n", argv[1]);
    printf("Expect: consistently HIGH scores (these are provably the same pattern).\n");
    double sameSum = 0; int sameN = 0, sameMin = 1;
    for (int t = 0; t < NUM_TRANSFORMS; t++) {
        unsigned char *variant = malloc((size_t)n);
        transform_image(base, rows, cols, TRANSFORMS[t].dx, TRANSFORMS[t].dy, TRANSFORMS[t].angle, variant);
        int cand, inliers;
        float score = verify_pair(base, rows, cols, variant, octaves, &cand, &inliers);
        printf("  %-28s score=%.4f (candidates=%d, inliers=%d)\n", TRANSFORMS[t].label, score, cand, inliers);
        sameSum += score; sameN++;
        free(variant);
        (void)sameMin;
    }
    printf("  same-image avg score = %.4f\n\n", sameSum / sameN);

    printf("=== STEP 2: synthetic different-image test ===\n");
    printf("Expect: consistently LOW scores (these are genuinely different fingers/transforms).\n");
    double diffSum = 0; int diffN = 0;
    for (int i = 2; i < argc; i++) {
        unsigned char *other = load_raw_be16_as_u8(argv[i], n);
        int cand, inliers;
        float score0 = verify_pair(base, rows, cols, other, octaves, &cand, &inliers);
        printf("  %s (base) vs %s (raw, no transform): score=%.4f (candidates=%d, inliers=%d)\n",
               argv[1], argv[i], score0, cand, inliers);
        diffSum += score0; diffN++;

        for (int t = 1; t < NUM_TRANSFORMS; t++) { /* skip identity here, redundant with above */
            unsigned char *otherVariant = malloc((size_t)n);
            transform_image(other, rows, cols, TRANSFORMS[t].dx, TRANSFORMS[t].dy, TRANSFORMS[t].angle, otherVariant);
            float scoreBaseVsOtherVariant = verify_pair(base, rows, cols, otherVariant, octaves, &cand, &inliers);
            printf("  %s (base) vs %s+%-28s score=%.4f (candidates=%d, inliers=%d)\n",
                   argv[1], argv[i], TRANSFORMS[t].label, scoreBaseVsOtherVariant, cand, inliers);
            diffSum += scoreBaseVsOtherVariant; diffN++;
            free(otherVariant);
        }
        free(other);
    }
    printf("  different-image avg score = %.4f\n\n", diffSum / diffN);

    printf("=== SUMMARY ===\n");
    printf("  same-image (synthetic transform of itself):  avg=%.4f  n=%d\n", sameSum / sameN, sameN);
    printf("  different-image (genuinely different finger): avg=%.4f  n=%d\n", diffSum / diffN, diffN);
    printf("  separation (same_avg - diff_avg) = %.4f\n", sameSum / sameN - diffSum / diffN);

    free(base);
    return 0;
}
