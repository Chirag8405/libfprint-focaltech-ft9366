/* Diff our reimplementation's keypoints against the real ground truth
 * (research/ground_truth/*_real.txt), on the SAME source image, using
 * our own 96x96-canvas coordinate space (both should now agree on this
 * per the confirmed geometry fix) for a direct, apples-to-apples
 * comparison. For each of our keypoints, find the nearest real keypoint
 * and report the distance -- localizes whether detection finds the same
 * real locations (just a different subset) or is genuinely off. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

typedef struct { float x, y, ori; unsigned int desc[8]; } RealFeat;

static int load_real(const char *path, RealFeat **out)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    RealFeat *feats = malloc(sizeof(RealFeat) * 500);
    int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        int which, idx;
        float x, y, ori;
        unsigned int d[8];
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

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <capture.raw> <ground_truth_real.txt>\n", argv[0]);
        return 1;
    }
    const int rows = 80, cols = 64, octaves = 4;
    unsigned char *img = load_raw_be16_as_u8(argv[1], rows * cols);
    FocalFeature *mine;
    int nMine = focal_extract_features(img, rows, cols, octaves, &mine);

    RealFeat *real;
    int nReal = load_real(argv[2], &real);

    printf("mine: %d features, real: %d features\n", nMine, nReal);
    printf("\n=== for each of MY keypoints, nearest REAL keypoint distance + descriptor Hamming ===\n");
    double sumDist = 0;
    int within3px = 0;
    for (int i = 0; i < nMine; i++) {
        double best = 1e9; int bestj = -1;
        for (int j = 0; j < nReal; j++) {
            double dx = mine[i].x - real[j].x, dy = mine[i].y - real[j].y;
            double d = sqrt(dx*dx + dy*dy);
            if (d < best) { best = d; bestj = j; }
        }
        int hdist = bestj >= 0 ? focal_hamming_distance(mine[i].desc, real[bestj].desc) : -1;
        printf("  mine[%2d] (%.1f,%.1f,ori=%.2f) -> nearest real[%2d] (%.1f,%.1f,ori=%.2f) dist=%.2fpx hamming=%d\n",
               i, mine[i].x, mine[i].y, mine[i].ori, bestj, real[bestj].x, real[bestj].y, real[bestj].ori, best, hdist);
        sumDist += best;
        if (best <= 3.0) within3px++;
    }
    printf("\navg nearest-distance = %.2fpx, %d/%d (%.0f%%) of my keypoints within 3px of some real keypoint\n",
           sumDist / nMine, within3px, nMine, 100.0 * within3px / nMine);
    return 0;
}
