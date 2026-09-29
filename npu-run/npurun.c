/*
 * npurun.c -- minimal NBG inference runner for the A733 NPU (vip_lite API).
 *   ./npurun model.nb [input.dat [loops]]
 * Input file is dumped raw into the input tensor (vpm_run's input_0.dat
 * works). No OpenCV. Built for aarch64; links libNBGlinker + libVIPhal.
 */
#include <vip_lite.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHK(stmt) do { \
	vip_status_e _s = (stmt); \
	if (_s != VIP_SUCCESS) { \
		fprintf(stderr, "npurun: VIP error %d: %s (%s:%d)\n", \
			_s, #stmt, __FILE__, __LINE__); \
		return 1; \
	} \
} while (0)

#define CHKP(stmt) do { \
	vip_status_e _s = (stmt); \
	if (_s != VIP_SUCCESS) { \
		fprintf(stderr, "npurun: VIP error %d: %s (%s:%d)\n", \
			_s, #stmt, __FILE__, __LINE__); \
		return NULL; \
	} \
} while (0)

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Query dims of an input/output and create a matching vip_buffer.
 * SIZES_OF_DIMENSION fills a caller-provided u32 array (learned the
 * hard way: passing &ptr gets the array written over the pointer). */
static vip_buffer make_tensor_buffer(vip_network net, vip_uint32_t index,
				     int is_output)
{
	vip_buffer_create_params_t p;
	vip_buffer buf = NULL;
	vip_uint32_t ndims = 0, sizes[6] = { 0 };
	vip_enum fmt = 0, quant = 0;
	vip_uint32_t i;

	memset(&p, 0, sizeof(p));

	if (is_output) {
		CHKP(vip_query_output(net, index,
			VIP_BUFFER_PROP_NUM_OF_DIMENSION, &ndims));
		CHKP(vip_query_output(net, index,
			VIP_BUFFER_PROP_SIZES_OF_DIMENSION, sizes));
		CHKP(vip_query_output(net, index,
			VIP_BUFFER_PROP_DATA_FORMAT, &fmt));
		CHKP(vip_query_output(net, index,
			VIP_BUFFER_PROP_QUANT_FORMAT, &quant));
	} else {
		CHKP(vip_query_input(net, index,
			VIP_BUFFER_PROP_NUM_OF_DIMENSION, &ndims));
		CHKP(vip_query_input(net, index,
			VIP_BUFFER_PROP_SIZES_OF_DIMENSION, sizes));
		CHKP(vip_query_input(net, index,
			VIP_BUFFER_PROP_DATA_FORMAT, &fmt));
		CHKP(vip_query_input(net, index,
			VIP_BUFFER_PROP_QUANT_FORMAT, &quant));
	}

	if (!ndims || ndims > 6) {
		fprintf(stderr, "npurun: bad dim count %u\n", ndims);
		return NULL;
	}

	p.num_of_dims = ndims;
	for (i = 0; i < ndims; i++)
		p.sizes[i] = sizes[i] ? sizes[i] : 1;
	p.data_format = fmt;
	p.quant_format = quant;

	CHKP(vip_create_buffer(&p, sizeof(p), &buf));
	fprintf(stderr, "npurun: %s %u buffer: %ux%ux%ux%u fmt=%d quant=%d\n",
		is_output ? "output" : "input", index,
		sizes[0], sizes[1], sizes[2], sizes[3], (int)fmt, (int)quant);
	return buf;
}

int main(int argc, char **argv)
{
	const char *model = argc > 1 ? argv[1] : "model.nb";
	const char *input = argc > 2 ? argv[2] : NULL;
	int loops = argc > 3 ? atoi(argv[3]) : 10;
	vip_network net = NULL;
	vip_buffer inbuf = NULL;
	vip_buffer outbuf[8] = { 0 };
	vip_uint32_t out_count = 0, i;
	double t_best = 1e9, t_sum = 0;
	FILE *fp;

	if (loops < 1)
		loops = 1;

	CHK(vip_init());
	CHK(vip_create_network(model, 0, VIP_CREATE_NETWORK_FROM_FILE, &net));
	CHK(vip_prepare_network(net));

	inbuf = make_tensor_buffer(net, 0, 0);
	if (!inbuf)
		return 1;

	if (input) {
		void *m = vip_map_buffer(inbuf);
		vip_uint32_t sz = vip_get_buffer_size(inbuf);

		fp = fopen(input, "rb");
		if (!fp) {
			fprintf(stderr, "npurun: cannot open %s\n", input);
			return 1;
		}
		if (fread(m, 1, sz, fp) == 0 && sz)
			fprintf(stderr, "npurun: short input, rest is zero\n");
		fclose(fp);
		CHK(vip_flush_buffer(inbuf, VIP_BUFFER_OPER_TYPE_FLUSH));
	}

	CHK(vip_set_input(net, 0, inbuf));

	CHK(vip_query_network(net, VIP_NETWORK_PROP_OUTPUT_COUNT, &out_count));
	if (out_count > 8)
		out_count = 8;
	for (i = 0; i < out_count; i++) {
		outbuf[i] = make_tensor_buffer(net, i, 1);
		if (!outbuf[i])
			return 1;
		CHK(vip_set_output(net, i, outbuf[i]));
	}

	for (i = 0; i < loops + 1; i++) {
		double t0, t1;
		vip_uint32_t k;

		t0 = now_ms();
		CHK(vip_run_network(net));
		t1 = now_ms();

		for (k = 0; k < out_count; k++)
			CHK(vip_flush_buffer(outbuf[k],
					     VIP_BUFFER_OPER_TYPE_INVALIDATE));

		if (i == 0)
			continue;		/* warm-up */
		if (t1 - t0 < t_best)
			t_best = t1 - t0;
		t_sum += t1 - t0;
	}

	printf("npurun: %s  loops=%d  avg=%.2f ms  min=%.2f ms\n",
	       model, loops, t_sum / loops, t_best);

	for (i = 0; i < out_count; i++) {
		void *m = vip_map_buffer(outbuf[i]);
		vip_uint32_t sz = vip_get_buffer_size(outbuf[i]);
		vip_uint32_t k, n = sz / 4 < 5 ? sz / 4 : 5;

		CHK(vip_flush_buffer(outbuf[i], VIP_BUFFER_OPER_TYPE_INVALIDATE));
		printf("npurun: out[%u] size=%u first:", i, sz);
		for (k = 0; k < n; k++)
			printf(" %d", ((int *)m)[k]);
		printf("\n");
	}

	vip_destroy();
	return 0;
}
