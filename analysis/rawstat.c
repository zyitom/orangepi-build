/*
 * rawstat: per-frame channel means of a SAVEALL RAW10 (GRBG, 16-bit LE) dump.
 * rawstat <file> <w> <h> [fps]
 * prints: frame t  | full: R G B R/G B/G | left third | right third  (black 42 removed,
 * pixels >= 1000 excluded)
 */
#include <stdio.h>
#include <stdlib.h>

#define BL 42

int main(int argc, char **argv)
{
	int w = atoi(argv[2]), h = atoi(argv[3]);
	double fps = argc > 4 ? atof(argv[4]) : 30;
	FILE *f = fopen(argv[1], "rb");
	unsigned short *p = malloc((size_t)w * h * 2);
	int n = 0;

	while (fread(p, 2, (size_t)w * h, f) == (size_t)w * h) {
		/* region 0 full, 1 left third, 2 right third; channel 0 R 1 G 2 B */
		double s[3][3] = {{0}};
		long c[3][3] = {{0}};
		for (int y = 0; y < h - 1; y += 2) {
			for (int x = 0; x < w - 1; x += 2) {
				int gr = p[y * w + x], r = p[y * w + x + 1];
				int b = p[(y + 1) * w + x], gb = p[(y + 1) * w + x + 1];
				if (gr >= 1000 || r >= 1000 || b >= 1000 || gb >= 1000)
					continue;
				int reg[2] = { 0, x < w / 3 ? 1 : (x >= 2 * w / 3 ? 2 : -1) };
				for (int k = 0; k < 2; k++) {
					int g = reg[k];
					if (g < 0)
						continue;
					s[g][0] += r - BL; s[g][1] += (gr + gb) / 2.0 - BL; s[g][2] += b - BL;
					c[g][0]++; c[g][1]++; c[g][2]++;
				}
			}
		}
		printf("%4d %6.2f", n, n / fps);
		for (int g = 0; g < 3; g++) {
			double R = s[g][0] / c[g][0], G = s[g][1] / c[g][1], B = s[g][2] / c[g][2];
			printf(" | %6.1f %6.1f %6.1f %.3f %.3f", R, G, B, R / G, B / G);
		}
		printf("\n");
		n++;
	}
	return 0;
}
