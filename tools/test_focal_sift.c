/* Smoke test / first real-data run for the Step 2-3 reimplementation
 * (focal_sift.c). Loads real FT9366 captures (16-bit BE raw, 64x80),
 * runs the full detect+describe pipeline, and reports feature counts
 * plus Hamming-distance statistics between capture pairs -- purely
 * diagnostic at this stage, not a claim of a working matcher yet. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, ori; unsigned int desc[8]; } FocalFeature;
extern int focal_extract_features(const unsigned char *img, int rows, int cols,
                                   int octaves, FocalFeature **out_features);
extern int focal_hamming_distance(const unsigned int a[8], const unsigned int b[8]);

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
    FocalFeature **feats = calloc((size_t)n, sizeof(FocalFeature *));
    int *counts = calloc((size_t)n, sizeof(int));
    int i, j;

    for (i = 0; i < n; i++) {
        unsigned char *img = load_raw_be16_as_u8(argv[i + 1], rows * cols);
        counts[i] = focal_extract_features(img, rows, cols, octaves, &feats[i]);
        printf("%s: %d features\n", argv[i + 1], counts[i]);
        free(img);
    }

    printf("\n=== best-match Hamming distance per pair (out of 256 bits; lower = more similar) ===\n");
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (counts[i] == 0 || counts[j] == 0) {
                printf("  %s vs %s: no features to compare\n", argv[i + 1], argv[j + 1]);
                continue;
            }
            /* For each feature in i, find nearest neighbor in j, average
             * the best-match distances -- a crude first diagnostic, not
             * the real matcher (RANSAC+affine+FtCalcSimScore, Step 4). */
            long sum = 0;
            int matched = 0;
            for (int a = 0; a < counts[i]; a++) {
                int best = 256;
                for (int b = 0; b < counts[j]; b++) {
                    int d = focal_hamming_distance(feats[i][a].desc, feats[j][b].desc);
                    if (d < best) best = d;
                }
                sum += best;
                matched++;
            }
            printf("  %s vs %s: avg best-Hamming = %.1f (n=%d features in %s)\n",
                   argv[i + 1], argv[j + 1], matched ? (double)sum / matched : -1.0,
                   matched, argv[i + 1]);
        }
    }
    return 0;
}
