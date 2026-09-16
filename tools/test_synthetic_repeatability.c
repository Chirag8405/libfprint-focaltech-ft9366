/* Detection-only repeatability under a KNOWN EXACT synthetic transform
 * (companion to test_detection_repeatability.c, which measures repeatability
 * between independently captured real touches). Since the transform here is
 * exact (no independent second touch, no estimation/search needed), this
 * isolates the detector's OWN rotation/translation sensitivity from
 * additional real-world capture-to-capture noise (skin deformation,
 * pressure, moisture). See research/PROTOCOL.md ("Hypothesis (3)
 * CONFIRMED") for what this was used to establish: even with zero real-
 * world noise, this reimplementation's detector loses substantial
 * repeatability under a few degrees of pure rotation (97.8% at identity
 * down to 64-84% by 2-5 degrees), and real recaptures of the same finger
 * score even lower (~58%) than this synthetic-clean baseline -- so both
 * detector-own instability and real-world noise contribute.
 *
 * Build: gcc -O2 -Wall test_synthetic_repeatability.c focal_sift.c -o test_synthetic_repeatability -lm
 * Usage: test_synthetic_repeatability capture.raw [thresh_px]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef struct { float x, y, ori; unsigned int desc[8]; } FocalFeature;
extern int focal_extract_features(const unsigned char *img, int rows, int cols, int octaves, FocalFeature **out_features);
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
/* KNOWN transform between base and variant: dx,dy,theta (as passed to transform_image).
 * Since ground truth is exact here (unlike real recaptures), we don't need a search --
 * just apply the SAME transform to base's keypoints and check match rate directly. */
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s capture.raw [thresh_px]\n", argv[0]); return 1; }
    const int rows=80, cols=64, octaves=4, n=rows*cols;
    double thresh = argc>2 ? atof(argv[2]) : 3.0;
    unsigned char *base = load_raw_be16_as_u8(argv[1], n);
    FocalFeature *fa;
    int na = focal_extract_features(base, rows, cols, octaves, &fa);
    double cx=cols/2.0, cy=rows/2.0;
    double transforms[][3] = { {0,0,0}, {2,0,0}, {0,2,0}, {0,0,2}, {0,0,3}, {0,0,5}, {2,1,3}, {-1,2,-2} };
    for (int t=0; t<8; t++) {
        double dx=transforms[t][0], dy=transforms[t][1], deg=transforms[t][2];
        unsigned char *variant = malloc(n);
        transform_image(base, rows, cols, dx, dy, deg, variant);
        FocalFeature *fb;
        int nb = focal_extract_features(variant, rows, cols, octaves, &fb);
        double rad = deg * M_PI/180.0, c=cos(rad), s=sin(rad);
        int matched = 0;
        for (int i=0;i<na;i++) {
            double best=1e18;
            for (int j=0;j<nb;j++) {
                /* predict where fa[i] should land in variant's coord space using the KNOWN exact transform */
                double px = fa[i].x - cx, py = fa[i].y - cy;
                double predx = px*c - py*s + cx + dx;
                double predy = px*s + py*c + cy + dy;
                double ddx = predx - fb[j].x, ddy = predy - fb[j].y;
                double d2 = ddx*ddx+ddy*ddy;
                if (d2<best) best=d2;
            }
            if (best <= thresh*thresh) matched++;
        }
        printf("dx=%.0f dy=%.0f theta=%.0fdeg: na=%d nb=%d matched=%d rate=%.1f%% (thresh=%.1fpx, EXACT known transform)\n",
               dx, dy, deg, na, nb, matched, 100.0*matched/na, thresh);
        free(variant); free(fb);
    }
    return 0;
}
