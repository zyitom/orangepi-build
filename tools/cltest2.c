/* cltest2: enumerate OpenCL device + extensions (look for DMA-BUF import). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef int32_t cl_int; typedef uint32_t cl_uint; typedef uint64_t cl_ulong;
typedef void *cl_platform_id; typedef void *cl_device_id;
extern cl_int clGetPlatformIDs(cl_uint, cl_platform_id *, cl_uint *);
extern cl_int clGetPlatformInfo(cl_platform_id, cl_uint, size_t, void *, size_t *);
extern cl_int clGetDeviceIDs(cl_platform_id, cl_ulong, cl_uint, cl_device_id *, cl_uint *);
extern cl_int clGetDeviceInfo(cl_device_id, cl_uint, size_t, void *, size_t *);

static const char *want[] = {
	"cl_img_import_dma_buf", "cl_img_import_dma_buf_separate",
	"cl_arm_import_memory", "cl_ext_dma_buf", "cl_khr_image2d_from_buffer",
	"cl_img_yuv_image", "cl_khr_il_program",
};

int main(void)
{
	cl_platform_id p[4]; cl_uint n = 0; char s[4096];
	cl_int r = clGetPlatformIDs(4, p, &n);
	printf("platforms: %u (rc %d)\n", n, r);
	for (cl_uint i = 0; i < n; i++) {
		clGetPlatformInfo(p[i], 0x0902, sizeof s, s, NULL); printf("platform: %s\n", s);
		clGetPlatformInfo(p[i], 0x0901, sizeof s, s, NULL); printf("version : %s\n", s);
		cl_device_id d; cl_uint nd = 0;
		if (clGetDeviceIDs(p[i], 0xFFFFFFFF, 1, &d, &nd) || !nd)
			continue;
		clGetDeviceInfo(d, 0x102B, sizeof s, s, NULL); printf("device  : %s\n", s);
		memset(s, 0, sizeof s);
		if (!clGetDeviceInfo(d, 0x1027 /* EXTENSIONS */, sizeof s - 1, s, NULL)) {
			printf("extensions hit:\n");
			for (unsigned k = 0; k < sizeof want / sizeof want[0]; k++)
				if (strstr(s, want[k]))
					printf("  [x] %s\n", want[k]);
			char *tok = strtok(s, " ");
			int cnt = 0;
			printf("all (%s):\n  ", strstr(s, "cl_img") ? "see cl_img*" : "no cl_img");
			while (tok && cnt++ < 64) { printf("%s ", tok); tok = strtok(NULL, " "); }
			printf("\n");
		}
	}
	return 0;
}
