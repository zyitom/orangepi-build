/*
 * Software ISP for AR0234 RAW10 (GRBG, 16-bit LE, 1920x1200) using the
 * Kurokesu libcamera tuning: black level, AWB ct curve, CCM, gamma.
 *
 * swisp <raw_in> <nframes> <out.y4m> [first_frame.ppm]
 * Output: 960x600 I420 Y4M (2x2 bin demosaic), 30fps.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ar0234_tuning.h"

#define IW 1920
#define IH 1200
#define OW (IW / 2)
#define OH (IH / 2)

static double interp_ct(double t, int col)
{
	if (t <= CT_CURVE[0][0])
		return CT_CURVE[0][col];
	for (int i = 1; i < N_CT; i++)
		if (t <= CT_CURVE[i][0]) {
			double a = (t - CT_CURVE[i - 1][0]) / (CT_CURVE[i][0] - CT_CURVE[i - 1][0]);
			return CT_CURVE[i - 1][col] * (1 - a) + CT_CURVE[i][col] * a;
		}
	return CT_CURVE[N_CT - 1][col];
}

static void interp_ccm(double t, double *m)
{
	int i;
	if (t <= CCM_CT[0]) { memcpy(m, CCM[0], sizeof(double) * 9); return; }
	if (t >= CCM_CT[N_CCM - 1]) { memcpy(m, CCM[N_CCM - 1], sizeof(double) * 9); return; }
	for (i = 1; i < N_CCM; i++)
		if (t <= CCM_CT[i])
			break;
	double a = (t - CCM_CT[i - 1]) / (CCM_CT[i] - CCM_CT[i - 1]);
	for (int k = 0; k < 9; k++)
		m[k] = CCM[i - 1][k] * (1 - a) + CCM[i][k] * a;
}

static unsigned char gamma_lut[4096];

static void build_gamma(void)
{
	for (int i = 0; i < 4096; i++) {
		double x = i * 65535.0 / 4095, y = 65535;
		for (int k = 1; k < N_GAMMA; k++)
			if (x <= GAMMA[k][0]) {
				double a = (x - GAMMA[k - 1][0]) / (GAMMA[k][0] - GAMMA[k - 1][0]);
				y = GAMMA[k - 1][1] * (1 - a) + GAMMA[k][1] * a;
				break;
			}
		gamma_lut[i] = (unsigned char)(y / 257.0 + 0.5);
	}
}

static inline double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s raw nframes out.y4m [first.ppm]\n", argv[0]);
		return 1;
	}
	FILE *in = fopen(argv[1], "rb");
	int n = atoi(argv[2]);
	FILE *out = fopen(argv[3], "wb");
	if (!in || !out) { perror("open"); return 1; }
	build_gamma();

	double black = BLACK_LEVEL_16 / 64.0, white = 1023.0 - black;
	static unsigned short raw[IW * IH];
	static float rgb[OW * OH * 3];
	static unsigned char y[OW * OH], u[OW * OH / 4], v[OW * OH / 4];
	double ct_s = 0, dg_s = 0;

	fprintf(out, "YUV4MPEG2 W%d H%d F30:1 Ip A1:1 C420jpeg\n", OW, OH);
	for (int f = 0; f < n; f++) {
		if (fread(raw, 2, IW * IH, in) != IW * IH)
			break;
		double sr = 0, sg = 0, sb = 0;
		long cnt = 0;
		/* GRBG: (0,0)=Gr (0,1)=R (1,0)=B (1,1)=Gb, 2x2 bin demosaic */
		for (int j = 0; j < OH; j++)
			for (int i = 0; i < OW; i++) {
				unsigned short *p = raw + (2 * j) * IW + 2 * i;
				double gr = p[0], r = p[1], b = p[IW], gb = p[IW + 1];
				double R = (r - black) / white, G = ((gr + gb) / 2 - black) / white,
				       B = (b - black) / white;
				float *o = rgb + (j * OW + i) * 3;
				o[0] = R; o[1] = G; o[2] = B;
				/* grey world on unsaturated, not-too-dark pixels */
				if (r < 1000 && gr < 1000 && b < 1000 && G > 0.01) {
					sr += R; sg += G; sb += B; cnt++;
				}
			}
		/* AWB: nearest point on the calibrated ct curve to grey-world ratios */
		double rgw = cnt ? sr / sg : 1, bgw = cnt ? sb / sg : 1, best_t = 5000, best_d = 1e9;
		for (double t = AWB_LO; t <= AWB_HI; t += 10) {
			double dr = interp_ct(t, 1) - rgw, db = interp_ct(t, 2) - bgw;
			double d = dr * dr + db * db;
			if (d < best_d) { best_d = d; best_t = t; }
		}
		ct_s = f ? ct_s * 0.8 + best_t * 0.2 : best_t;
		double gR = 1.0 / interp_ct(ct_s, 1), gB = 1.0 / interp_ct(ct_s, 2), m[9];
		interp_ccm(ct_s, m);

		/* digital exposure normalisation: mean linear luma -> 0.18 */
		double ysum = 0;
		for (int k = 0; k < OW * OH; k += 7) {
			float *o = rgb + k * 3;
			ysum += 0.299 * o[0] * gR + 0.587 * o[1] + 0.114 * o[2] * gB;
		}
		double ymean = ysum / (OW * OH / 7.0);
		double dg = ymean > 1e-4 ? 0.18 / ymean : 1;
		if (dg < 1) dg = 1;
		if (dg > 8) dg = 8;
		dg_s = f ? dg_s * 0.8 + dg * 0.2 : dg;

		for (int k = 0; k < OW * OH; k++) {
			float *o = rgb + k * 3;
			double R = o[0] * gR * dg_s, G = o[1] * dg_s, B = o[2] * gB * dg_s;
			double r2 = clamp01(m[0] * R + m[1] * G + m[2] * B);
			double g2 = clamp01(m[3] * R + m[4] * G + m[5] * B);
			double b2 = clamp01(m[6] * R + m[7] * G + m[8] * B);
			o[0] = gamma_lut[(int)(r2 * 4095)] / 255.0;
			o[1] = gamma_lut[(int)(g2 * 4095)] / 255.0;
			o[2] = gamma_lut[(int)(b2 * 4095)] / 255.0;
		}
		if (f == 0 && argc > 4) {
			FILE *pp = fopen(argv[4], "wb");
			fprintf(pp, "P6\n%d %d\n255\n", OW, OH);
			for (int k = 0; k < OW * OH * 3; k++)
				fputc((int)(rgb[k] * 255 + 0.5), pp);
			fclose(pp);
		}
		/* BT.601 limited range I420 */
		for (int j = 0; j < OH; j++)
			for (int i = 0; i < OW; i++) {
				float *o = rgb + (j * OW + i) * 3;
				y[j * OW + i] = 16 + 65.481 * o[0] + 128.553 * o[1] + 24.966 * o[2];
			}
		for (int j = 0; j < OH / 2; j++)
			for (int i = 0; i < OW / 2; i++) {
				double R = 0, G = 0, B = 0;
				for (int dy = 0; dy < 2; dy++)
					for (int dx = 0; dx < 2; dx++) {
						float *o = rgb + ((2 * j + dy) * OW + 2 * i + dx) * 3;
						R += o[0] / 4; G += o[1] / 4; B += o[2] / 4;
					}
				u[j * OW / 2 + i] = 128 - 37.797 * R - 74.203 * G + 112.0 * B;
				v[j * OW / 2 + i] = 128 + 112.0 * R - 93.786 * G - 18.214 * B;
			}
		fputs("FRAME\n", out);
		fwrite(y, 1, OW * OH, out);
		fwrite(u, 1, OW * OH / 4, out);
		fwrite(v, 1, OW * OH / 4, out);
		if (f % 15 == 0)
			fprintf(stderr, "frame %d: grey-world r/g=%.3f b/g=%.3f -> ct=%.0fK gains R=%.2f B=%.2f dgain=%.2f\n",
				f, rgw, bgw, ct_s, gR, gB, dg_s);
	}
	fclose(out);
	return 0;
}
