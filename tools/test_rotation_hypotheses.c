/* Quick experiment: try several rotation/sign conventions for the steered
 * descriptor sampling and see which minimizes Hamming distance at
 * position+orientation-matched real ground-truth keypoints. Diagnostic
 * only, not part of the shipped reimplementation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "focal_tables.h"

/* NOTE: this experiment samples from the plain padded capture image, not
 * the correct Gaussian-pyramid level (octave/interval-specific blur) the
 * real descriptor is actually sampled from -- an approximation used only
 * to compare rotation CONVENTIONS against each other cheaply. Absolute
 * Hamming values here are not expected to hit 0; only the RELATIVE
 * ranking across hypotheses is meaningful. */

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

typedef struct { float x, y, ori; unsigned int desc[8]; } RealFeat;
static int load_real(const char *path, RealFeat **out)
{
    FILE *f = fopen(path, "r");
    RealFeat *feats = malloc(sizeof(RealFeat) * 500);
    int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        int which, idx; float x, y, ori; unsigned int d[8];
        int nf = sscanf(line, "%d %d %f %f %f %x %x %x %x %x %x %x %x",
                         &which, &idx, &x, &y, &ori, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]);
        if (nf != 13) continue;
        feats[n].x = x; feats[n].y = y; feats[n].ori = ori;
        memcpy(feats[n].desc, d, sizeof(d));
        n++;
    }
    fclose(f);
    *out = feats;
    return n;
}

static float bilinear(const unsigned char *img, int rows, int cols, float x, float y)
{
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x0 < 0) x0 = 0; if (x0 >= cols) x0 = cols - 1;
    if (x1 < 0) x1 = 0; if (x1 >= cols) x1 = cols - 1;
    if (y0 < 0) y0 = 0; if (y0 >= rows) y0 = rows - 1;
    if (y1 < 0) y1 = 0; if (y1 >= rows) y1 = rows - 1;
    float v00 = img[y0*cols+x0], v01 = img[y0*cols+x1], v10 = img[y1*cols+x0], v11 = img[y1*cols+x1];
    return v00*(1-fx)*(1-fy) + v01*fx*(1-fy) + v10*(1-fx)*fy + v11*fx*fy;
}

static int hamming(const unsigned int a[8], const unsigned int b[8])
{
    int d = 0;
    for (int i = 0; i < 8; i++) d += __builtin_popcount(a[i] ^ b[i]);
    return d;
}

typedef void (*RotFn)(float dx, float dy, float cos_o, float sin_o, float *rx, float *ry);
static void rot_ccw(float dx, float dy, float c, float s, float *rx, float *ry) { *rx = dx*c - dy*s; *ry = dx*s + dy*c; }
static void rot_cw(float dx, float dy, float c, float s, float *rx, float *ry) { *rx = dx*c + dy*s; *ry = -dx*s + dy*c; }
static void rot_ccw_negori(float dx, float dy, float c, float s, float *rx, float *ry) { *rx = dx*c + dy*s; *ry = -dx*s + dy*c; }
static void rot_swapxy(float dx, float dy, float c, float s, float *rx, float *ry) { *ry = dx*c - dy*s; *rx = dx*s + dy*c; }
static void rot_none(float dx, float dy, float c, float s, float *rx, float *ry) { (void)c;(void)s; *rx = dx; *ry = dy; }

static void compute_desc(const unsigned char *img, int rows, int cols, float x, float y, float ori,
                          RotFn rotFn, unsigned int desc[8])
{
    float cos_o = cosf(ori), sin_o = sinf(ori);
    float samples[FOCAL_NUM_SAMPLE_POINTS];
    for (int i = 0; i < FOCAL_NUM_SAMPLE_POINTS; i++) {
        float dx = g_coordinare_pairs[i][0], dy = g_coordinare_pairs[i][1];
        float rx, ry;
        rotFn(dx, dy, cos_o, sin_o, &rx, &ry);
        samples[i] = bilinear(img, rows, cols, x + rx, y + ry);
    }
    memset(desc, 0, 8*sizeof(unsigned int));
    for (int i = 0; i < FOCAL_NUM_DESCRIPTOR_BITS; i++) {
        int a = g_mode_pairs[i][0], b = g_mode_pairs[i][1];
        if (samples[a] < samples[b]) desc[i/32] |= (1u << (i%32));
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s capture.raw ground_truth_real.txt\n", argv[0]); return 1; }
    const int rows = 80, cols = 64, canvas = 96;
    unsigned char *img_tight = load_raw_be16_as_u8(argv[1], rows*cols);
    unsigned char *canvasImg = calloc(1, canvas*canvas);
    int padRows = (canvas-rows)/2, padCols = (canvas-cols)/2;
    for (int r = 0; r < rows; r++)
        memcpy(canvasImg + (r+padRows)*canvas + padCols, img_tight + r*cols, cols);

    RealFeat *real; int nReal = load_real(argv[2], &real);

    struct { const char *name; RotFn fn; } hyps[] = {
        {"ccw (current)", rot_ccw}, {"cw", rot_cw}, {"ccw_negori(-ori)", rot_ccw_negori},
        {"swapxy", rot_swapxy}, {"none(no rotation)", rot_none},
    };
    for (int h = 0; h < 5; h++) {
        long sum = 0;
        for (int j = 0; j < nReal; j++) {
            unsigned int desc[8];
            compute_desc(canvasImg, canvas, canvas, real[j].x, real[j].y, real[j].ori, hyps[h].fn, desc);
            sum += hamming(desc, real[j].desc);
        }
        printf("%-22s avg Hamming vs real (using REAL x,y,ori as input) = %.1f/256\n", hyps[h].name, (double)sum / nReal);
    }
    return 0;
}
