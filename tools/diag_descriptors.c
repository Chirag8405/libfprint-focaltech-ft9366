/* Diagnostic: check whether some captures' own descriptors are
 * unusually low-diversity (many near-duplicate descriptors within the
 * SAME image), which would make them spuriously "match" almost anything
 * during RANSAC regardless of true finger identity. */
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
    const int rows=80, cols=64, octaves=4;
    for (int a = 1; a < argc; a++) {
        unsigned char *img = load_raw_be16_as_u8(argv[a], rows*cols);
        FocalFeature *f;
        int n = focal_extract_features(img, rows, cols, octaves, &f);

        long sum=0, cnt=0; int mn=256;
        for (int i = 0; i < n; i++)
            for (int j = i+1; j < n; j++) {
                int d = focal_hamming_distance(f[i].desc, f[j].desc);
                sum += d; cnt++;
                if (d < mn) mn = d;
            }
        double avg = cnt ? (double)sum/cnt : -1;

        /* ori/position spread as a sanity check too */
        float xmin=1e9,xmax=-1e9,ymin=1e9,ymax=-1e9;
        for (int i=0;i<n;i++){ if(f[i].x<xmin)xmin=f[i].x; if(f[i].x>xmax)xmax=f[i].x;
                                if(f[i].y<ymin)ymin=f[i].y;
                                if(f[i].y>ymax)ymax=f[i].y; }

        printf("%-55s n=%3d  intra-avgHamming=%.1f  intra-minHamming=%d  xrange=[%.0f,%.0f] yrange=[%.0f,%.0f]\n",
               argv[a], n, avg, mn, xmin, xmax, ymin, ymax);
        free(img); free(f);
    }
    return 0;
}
