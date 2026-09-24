/* CPU cost of NV12 -> RGB888 (BT.601 integer, -O3 autovectorised) and of a 2x resize, 1920x1200 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static inline uint8_t clip(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
int main(void)
{
	const int w = 1920, h = 1200, n = 60;
	uint8_t *nv12 = malloc(w * h * 3 / 2), *rgb = malloc(w * h * 3), *small = malloc(640 * 400 * 3);
	for (int i = 0; i < w * h * 3 / 2; i++) nv12[i] = (uint8_t)(i * 31);
	double t0 = now();
	for (int f = 0; f < n; f++)
		for (int y = 0; y < h; y++) {
			const uint8_t *Y = nv12 + y * w, *UV = nv12 + w * h + (y / 2) * w;
			uint8_t *d = rgb + y * w * 3;
			for (int x = 0; x < w; x++) {
				int c = Y[x] - 16, u = UV[x & ~1] - 128, v = UV[(x & ~1) + 1] - 128;
				d[3 * x] = clip((298 * c + 409 * v + 128) >> 8);
				d[3 * x + 1] = clip((298 * c - 100 * u - 208 * v + 128) >> 8);
				d[3 * x + 2] = clip((298 * c + 516 * u + 128) >> 8);
			}
		}
	double t1 = now();
	for (int f = 0; f < n; f++)
		for (int y = 0; y < 400; y++)
			for (int x = 0; x < 640; x++)
				for (int k = 0; k < 3; k++)
					small[(y * 640 + x) * 3 + k] = rgb[(y * 3 * w + x * 3) * 3 + k];
	double t2 = now();
	printf("NV12->RGB 1920x1200: %.1f ms/frame; nearest resize to 640x400: %.2f ms/frame\n",
	       (t1 - t0) * 1e3 / n, (t2 - t1) * 1e3 / n);
	return 0;
}
