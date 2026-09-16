/* End-to-end test of the full reimplemented pipeline (Steps 2-4) against
 * real FT9366 captures. Reports raw scores plainly -- this is the actual
 * test of whether the reimplementation works, not a diagnostic. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s file1.raw file2.raw [file3.raw ...]\n", argv[0]);
        return 1;
    }
    const int rows = 80, cols = 64, octaves = 3;
    int n = argc - 1;
    unsigned char **imgs = calloc((size_t)n, sizeof(unsigned char *));
    FocalFeature **feats = calloc((size_t)n, sizeof(FocalFeature *));
    int *counts = calloc((size_t)n, sizeof(int));
    int i, j;

    for (i = 0; i < n; i++) {
        imgs[i] = load_raw_be16_as_u8(argv[i + 1], rows * cols);
        counts[i] = focal_extract_features(imgs[i], rows, cols, octaves, &feats[i]);
        printf("%s: %d features\n", argv[i + 1], counts[i]);
    }

    printf("\n=== focal_verify_two_templates scores (0..1, higher = more similar) ===\n");
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            int inliers, candidates;
            float score = focal_verify_two_templates(
                feats[i], counts[i], imgs[i],
                feats[j], counts[j], imgs[j],
                rows, cols, &inliers, &candidates);
            printf("  %s vs %s: score=%.4f  (candidates=%d, RANSAC inliers=%d)\n",
                   argv[i + 1], argv[j + 1], score, candidates, inliers);
        }
    }
    return 0;
}
