/* Step 1 (this continuation): reproduce the real ground-truth keypoint's
 * 45 raw samples in our own code, sampling-only (no bit-comparison).
 *
 * We know the real keypoint's FINAL x,y,ori exactly (from
 * research/ground_truth/same5_feat0_real_samples.txt) but not which
 * octave/interval the real algorithm sampled from (that intermediate
 * state wasn't recoverable from the optimized real binary's locals).
 * Since local_x/local_y at candidate octave o = x * FOCAL_DBL_SCALE / 2^o
 * (inverting kp.x = (cc+xc)*2^o then /DBL_SCALE), we try every
 * octave/interval combination our own pyramid has and report which one's
 * sample pattern best matches the real array -- this determines the
 * right level empirically rather than guessing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "focal_tables.h"

typedef struct { int rows, cols; float *data; } FImage;
/* We need access to focal_sift.c's internal pyramid-building. Since those
 * are static, we replicate the minimal path here via the same exported
 * primitives is not available -- instead, link directly against a small
 * exposed test hook added to focal_sift.c: focal_debug_get_pyramid(). */
extern FImage ***focal_debug_get_pyramid(const unsigned char *img, int rows, int cols,
                                          int octaves, int *outIntvlsPlus3);
extern float focal_debug_img_get(FImage *im, int r, int c);

#define FOCAL_DBL_SCALE 2.0

static int cmp_u16(const void *a, const void *b)
{
    return (int)(*(const unsigned short *)a) - (int)(*(const unsigned short *)b);
}
static unsigned char *load_raw_be16_as_u8(const char *path, int npix)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = malloc((size_t)npix * 2);
    fread(buf, 1, (size_t)npix * 2, f); fclose(f);
    unsigned short *px = malloc((size_t)npix * sizeof(unsigned short));
    for (int i = 0; i < npix; i++) px[i] = (unsigned short)((buf[i*2]<<8)|buf[i*2+1]);
    free(buf);
    unsigned short *sorted = malloc((size_t)npix * sizeof(unsigned short));
    memcpy(sorted, px, (size_t)npix*sizeof(unsigned short));
    qsort(sorted, npix, sizeof(unsigned short), cmp_u16);
    unsigned short lo = sorted[(int)(npix*0.01)], hi = sorted[(int)(npix*0.99)];
    free(sorted);
    if (hi <= lo) hi = lo+1;
    unsigned char *out = malloc((size_t)npix);
    for (int i = 0; i < npix; i++) { int v=px[i]; if(v<lo)v=lo; if(v>hi)v=hi; out[i]=((v-lo)*255)/(hi-lo); }
    free(px);
    return out;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s capture.raw\n", argv[0]); return 1; }
    const int rows = 80, cols = 64, canvas = 96, octaves = 4;

    /* Real ground truth for this keypoint (research/ground_truth/same5_feat0_real_samples.txt) */
    float realX = 76.935547f, realY = 86.108253f, realOri = 1.596882f;
    float realSamples[45] = {
        174.237015f,136.349258f,87.5420685f,75.4343109f,83.4466171f,85.9492111f,118.895088f,166.259384f,
        194.314972f,196.200134f,170.012772f,113.864525f,52.6684914f,23.9277744f,19.4697552f,18.3794804f,
        20.7782841f,18.1769085f,24.5965595f,41.3034935f,102.419464f,167.297485f,205.076538f,214.541595f,
        203.41333f,162.989243f,86.2227707f,34.3078117f,8.28964424f,2.18526626f,0.843087077f,1.09060895f,
        0.912313938f,1.15048611f,1.15283084f,0.93849045f,2.51244426f,5.16832924f,27.0560818f,102.358871f,
        183.973206f,212.983841f,216.045135f,215.920792f,204.963181f
    };

    unsigned char *img_tight = load_raw_be16_as_u8(argv[1], rows * cols);
    unsigned char *canvasImg = calloc(1, (size_t)canvas * canvas);
    int padRows = (canvas - rows) / 2, padCols = (canvas - cols) / 2;
    for (int r = 0; r < rows; r++)
        memcpy(canvasImg + (r + padRows) * canvas + padCols, img_tight + r * cols, cols);

    int intvlsPlus3 = 0;
    FImage ***pyr = focal_debug_get_pyramid(canvasImg, canvas, canvas, octaves, &intvlsPlus3);

    float cos_o = cosf(realOri), sin_o = sinf(realOri);
    double bestErr = 1e18; int bestO = -1, bestI = -1;

    for (int o = 0; o < octaves; o++) {
        float scaleToOctave = FOCAL_DBL_SCALE / (float)(1 << o);
        float lx = realX * scaleToOctave, ly = realY * scaleToOctave;
        for (int i = 0; i < intvlsPlus3; i++) {
            FImage *im = pyr[o][i];
            if (lx < 0 || lx >= im->cols || ly < 0 || ly >= im->rows) continue;
            float mySamples[45];
            for (int k = 0; k < FOCAL_NUM_SAMPLE_POINTS; k++) {
                float dx = g_coordinare_pairs[k][0], dy = g_coordinare_pairs[k][1];
                float rx = lx + (dx * cos_o - dy * sin_o);
                float ry = ly + (dx * sin_o + dy * cos_o);
                int ix = (int)lroundf(rx), iy = (int)lroundf(ry);
                mySamples[k] = (ix >= 0 && ix < im->cols && iy >= 0 && iy < im->rows)
                               ? focal_debug_img_get(im, iy, ix) : 0.0f;
            }
            double err = 0;
            for (int k = 0; k < 45; k++) { double d = mySamples[k] - realSamples[k]; err += d*d; }
            printf("octave=%d intvl=%d (local=%.2f,%.2f, imgSize=%dx%d): sum-sq-err=%.1f\n",
                   o, i, lx, ly, im->cols, im->rows, err);
            if (err < bestErr) { bestErr = err; bestO = o; bestI = i; }
        }
    }
    printf("\nBEST MATCH: octave=%d intvl=%d, sum-sq-err=%.1f\n", bestO, bestI, bestErr);

    /* Print the best-match sample array side by side with real for inspection */
    if (bestO >= 0) {
        float scaleToOctave = FOCAL_DBL_SCALE / (float)(1 << bestO);
        float lx = realX * scaleToOctave, ly = realY * scaleToOctave;
        FImage *im = pyr[bestO][bestI];
        printf("\nindex  mine      real      diff\n");
        for (int k = 0; k < FOCAL_NUM_SAMPLE_POINTS; k++) {
            float dx = g_coordinare_pairs[k][0], dy = g_coordinare_pairs[k][1];
            float rx = lx + (dx * cos_o - dy * sin_o);
            float ry = ly + (dx * sin_o + dy * cos_o);
            int ix = (int)lroundf(rx), iy = (int)lroundf(ry);
            float v = (ix >= 0 && ix < im->cols && iy >= 0 && iy < im->rows) ? focal_debug_img_get(im, iy, ix) : 0.0f;
            printf("%2d     %8.3f  %8.3f  %8.3f\n", k, v, realSamples[k], v - realSamples[k]);
        }
    }
    return 0;
}
