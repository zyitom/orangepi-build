/* cltest2: enumerate OpenCL device + extensions (look for DMA-BUF import). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
typedef int32_t cl_int; typedef uint32_t cl_uint; typedef uint64_t cl_ulong;
typedef void *cl_platform_id; typedef void *cl_device_id;
#include <dlfcn.h>
static cl_int (*clGetPlatformIDs)(cl_uint, cl_platform_id *, cl_uint *);
static cl_int (*clGetPlatformInfo)(cl_platform_id, cl_uint, size_t, void *, size_t *);
static cl_int (*clGetDeviceIDs)(cl_platform_id, cl_ulong, cl_uint, cl_device_id *, cl_uint *);
static cl_int (*clGetDeviceInfo)(cl_device_id, cl_uint, size_t, void *, size_t *);

static const char *want[] = {
	"cl_img_import_dma_buf", "cl_img_import_dma_buf_separate",
	"cl_arm_import_memory", "cl_ext_dma_buf", "cl_khr_image2d_from_buffer",
	"cl_img_yuv_image", "cl_khr_il_program",
};

int main(void)
{
	void *h = dlopen("/usr/lib/libPVROCL.so", RTLD_NOW);
	if (!h) h = dlopen("libOpenCL.so", RTLD_NOW);
	if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
	clGetPlatformIDs = dlsym(h, "clGetPlatformIDs");
	clGetPlatformInfo = dlsym(h, "clGetPlatformInfo");
	clGetDeviceIDs = dlsym(h, "clGetDeviceIDs");
	clGetDeviceInfo = dlsym(h, "clGetDeviceInfo");
	cl_platform_id p[4]; cl_uint n = 0; char s[4096];
	cl_int r = clGetPlatformIDs(4, p, &n);
	printf("platforms: %u (rc %d)\n", n, r);
	for (cl_uint i = 0; i < n; i++) {
		clGetPlatformInfo(p[i], 0x0902, sizeof s, s, NULL); printf("platform: %s\n", s);
		clGetPlatformInfo(p[i], 0x0901, sizeof s, s, NULL); printf("version : %s\n", s);
		{
			size_t psz = 0;
			char pext[8192];
			clGetPlatformInfo(p[i], 0x0904 /* PLATFORM_EXTENSIONS */, sizeof pext, pext, &psz);
			pext[psz < sizeof pext - 1 ? psz : sizeof pext - 1] = 0;
			printf("platform extensions (%zu): %s\n", psz, pext);
		}
		cl_device_id d; cl_uint nd = 0;
		if (clGetDeviceIDs(p[i], 0xFFFFFFFF, 1, &d, &nd) || !nd)
			continue;
		clGetDeviceInfo(d, 0x102B, sizeof s, s, NULL); printf("device  : %s\n", s);
		size_t extsz = 16384;
		char *ext = malloc(extsz);
		{
			cl_int erc = clGetDeviceInfo(d, 0x1027, extsz, ext, &extsz);
			ext[extsz < 16383 ? extsz : 16383] = 0;
			printf("ext query rc=%d\n", erc);
		}
		if (1) {
			printf("extensions (%zu bytes): [", extsz);
			for (size_t q = 0; q < extsz && q < 8; q++)
				printf("%02x ", (unsigned char)ext[q]);
			printf("] raw='%s'\n", extsz < 64 ? ext : "");
			printf("hit:\n");
			for (unsigned k = 0; k < sizeof want / sizeof want[0]; k++)
				if (strstr(ext, want[k]))
					printf("  [x] %s\n", want[k]);
			char *tok = strtok(ext, " ");
			int cnt = 0;
			while (tok && cnt++ < 80) { printf("%s ", tok); tok = strtok(NULL, " "); }
			printf("\n");
			free(ext);
		} else printf("extensions query failed (sz %zu)\n", extsz);
	}
	return 0;
}
