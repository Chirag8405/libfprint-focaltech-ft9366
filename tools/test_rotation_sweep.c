/* Companion to test_synthetic.c: sweeps pure-rotation angle (no translation)
 * of a real capture against itself, to characterize exactly how the full
 * pipeline's match score degrades as a function of rotation alone, on a
 * pair that is PROVABLY the same underlying pattern by construction. See
 * research/PROTOCOL.md ("synthetic diagnostic") for what this was used to
 * discriminate. Reusable for validating any future RANSAC/matching change:
 * a fix to rotation handling should flatten this curve out near 1.0 rather
 * than the current smooth falloff to ~0.75-0.87 by 5-10 degrees. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef struct { float x, y, ori; unsigned int desc[8]; } FocalFeature;
extern int focal_extract_features(const unsigned char *img, int rows, int cols, int octaves, FocalFeature **out_features);
extern float focal_verify_two_templates(const FocalFeature *A, int na, const unsigned char *imgA, const FocalFeature *B, int nb, const unsigned char *imgB, int rows, int cols, int *outInliers, int *outCandidates);
static int cmp_u16(const void *a, const void *b) { return (int)(*(const unsigned short *)a) - (int)(*(const unsigned short *)b); }
static unsigned char *load_raw_be16_as_u8(const char *path, int npix) {
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
static void transform_image(const unsigned char *src, int rows, int cols, double dx, double dy, double angleDeg, unsigned char *out) {
    double rad = angleDeg * M_PI / 180.0;
    double cosA = cos(rad), sinA = sin(rad);
    double cx = cols/2.0, cy = rows/2.0;
    for (int r=0;r<rows;r++) for (int c=0;c<cols;c++) {
        double ox=(c+0.5)-cx-dx, oy=(r+0.5)-cy-dy;
        double sx = ox*cosA + oy*sinA + cx - 0.5;
        double sy = -ox*sinA + oy*cosA + cy - 0.5;
        int x0=(int)floor(sx), y0=(int)floor(sy);
        double tx=sx-x0, ty=sy-y0, v=0.0;
        if (x0>=0 && x0+1<cols && y0>=0 && y0+1<rows) {
            double v00=src[y0*cols+x0], v01=src[y0*cols+x0+1];
            double v10=src[(y0+1)*cols+x0], v11=src[(y0+1)*cols+x0+1];
            double top=v00+tx*(v01-v00), bot=v10+tx*(v11-v10);
            v = top+ty*(bot-top);
        }
        out[r*cols+c] = (unsigned char)(v<0?0:(v>255?255:v));
    }
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s capture.raw\n", argv[0]); return 1; }
    const int rows=80, cols=64, octaves=4, n=rows*cols;
    unsigned char *base = load_raw_be16_as_u8(argv[1], n);
    double angles[] = {0.0,0.5,1.0,1.5,2.0,2.5,3.0,4.0,5.0,7.0,10.0};
    for (int i=0;i<11;i++) {
        unsigned char *variant = malloc(n);
        transform_image(base, rows, cols, 0,0, angles[i], variant);
        FocalFeature *fa,*fb;
        int na = focal_extract_features(base, rows, cols, octaves, &fa);
        int nb = focal_extract_features(variant, rows, cols, octaves, &fb);
        int cand, inliers;
        float score = focal_verify_two_templates(fa, na, base, fb, nb, variant, rows, cols, &inliers, &cand);
        printf("rot=%5.1fdeg  score=%.4f  candidates=%d inliers=%d (na=%d nb=%d)\n", angles[i], score, cand, inliers, na, nb);
        free(fa); free(fb); free(variant);
    }
    return 0;
}
