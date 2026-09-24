/*
 * nv12stat: per-frame colour stats of a raw NV12 stream (v4l2-ctl --stream-to).
 * nv12stat <file> <w> <h> [fps]
 * prints: frame t | full: Y R/G B/G | left third | right third   (BT.601 full-range RGB,
 * pixels with Y > 235 excluded)
 */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	int w = atoi(argv[2]), h = atoi(argv[3]);
	double fps = argc > 4 ? atof(argv[4]) : 30;
	size_t fsz = (size_t)w * h * 3 / 2;
	FILE *f = fopen(argv[1], "rb");
	unsigned char *p = malloc(fsz);
	int n = 0;

	while (fread(p, 1, fsz, f) == fsz) {
		double s[3][4] = {{0}};
		for (int y = 0; y < h; y += 2) {
			for (int x = 0; x < w; x += 2) {
				double Y = p[y * w + x];
				double U = p[w * h + (y / 2) * w + x] - 128.0;
				double V = p[w * h + (y / 2) * w + x + 1] - 128.0;
				if (Y > 235)
					continue;
				double R = Y + 1.402 * V, G = Y - 0.344 * U - 0.714 * V, B = Y + 1.772 * U;
				int reg[2] = { 0, x < w / 3 ? 1 : (x >= 2 * w / 3 ? 2 : -1) };
				for (int k = 0; k < 2; k++) {
					if (reg[k] < 0)
						continue;
					s[reg[k]][0] += Y; s[reg[k]][1] += R; s[reg[k]][2] += G; s[reg[k]][3] += B;
				}
			}
		}
		printf("%4d %6.2f", n, n / fps);
		for (int g = 0; g < 3; g++)
			printf(" | %5.1f %.3f %.3f", s[g][0] / (s[g][2] ? 1 : 1) /
			       ((double)(w / 2) * (h / 2) / (g ? 3 : 1)),
			       s[g][1] / s[g][2], s[g][3] / s[g][2]);
		printf("\n");
		n++;
	}
	return 0;
}
