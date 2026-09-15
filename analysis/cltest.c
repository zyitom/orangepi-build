/* list OpenCL platforms/devices without CL headers: cltest (via libOpenCL ICD loader or libPVROCL directly) */
#include <stdio.h>
#include <stdint.h>
typedef int32_t cl_int; typedef uint32_t cl_uint; typedef uint64_t cl_ulong;
typedef void *cl_platform_id; typedef void *cl_device_id;
extern cl_int clGetPlatformIDs(cl_uint, cl_platform_id *, cl_uint *);
extern cl_int clGetPlatformInfo(cl_platform_id, cl_uint, size_t, void *, size_t *);
extern cl_int clGetDeviceIDs(cl_platform_id, cl_ulong, cl_uint, cl_device_id *, cl_uint *);
extern cl_int clGetDeviceInfo(cl_device_id, cl_uint, size_t, void *, size_t *);
int main(void)
{
	cl_platform_id p[4]; cl_uint n = 0; char s[256];
	cl_int r = clGetPlatformIDs(4, p, &n);
	printf("clGetPlatformIDs = %d, platforms %u\n", r, n);
	for (cl_uint i = 0; i < n; i++) {
		clGetPlatformInfo(p[i], 0x0902 /* NAME */, sizeof s, s, NULL); printf(" platform: %s\n", s);
		clGetPlatformInfo(p[i], 0x0901 /* VERSION */, sizeof s, s, NULL); printf(" version : %s\n", s);
		cl_device_id d; cl_uint nd = 0;
		if (!clGetDeviceIDs(p[i], 0xFFFFFFFF, 1, &d, &nd) && nd) {
			clGetDeviceInfo(d, 0x102B /* NAME */, sizeof s, s, NULL); printf(" device  : %s\n", s);
			clGetDeviceInfo(d, 0x102F /* VERSION */, sizeof s, s, NULL); printf(" dev ver : %s\n", s);
		}
	}
	return 0;
}
