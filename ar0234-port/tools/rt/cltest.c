#define CL_TARGET_OPENCL_VERSION 300
#include <CL/cl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
static const char *src =
"__kernel void saxpy(__global const float *x, __global float *y, float a, int iters){"
" int i = get_global_id(0); float v = y[i];"
" for (int k = 0; k < iters; k++) v = a * x[i] + v * 0.5f;"
" y[i] = v; }";
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void){
  const int N = 1 << 22, IT = 64; const float A = 1.5f; cl_int e;
  float *x = malloc(N*4), *y = malloc(N*4), *ref = malloc(N*4);
  for (int i = 0; i < N; i++) { x[i] = (i % 1000) * 0.001f; y[i] = ref[i] = 1.0f; }
  double c0 = now();
  for (int i = 0; i < N; i++) { float v = ref[i]; for (int k = 0; k < IT; k++) v = A * x[i] + v * 0.5f; ref[i] = v; }
  double cpu = now() - c0;
  cl_platform_id p; cl_device_id d; clGetPlatformIDs(1, &p, NULL);
  clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, 1, &d, NULL);
  cl_context ctx = clCreateContext(NULL, 1, &d, NULL, NULL, &e);
  cl_command_queue q = clCreateCommandQueueWithProperties(ctx, d, NULL, &e);
  cl_program pr = clCreateProgramWithSource(ctx, 1, &src, NULL, &e);
  if (clBuildProgram(pr, 1, &d, NULL, NULL, NULL)) { printf("build failed\n"); return 1; }
  cl_kernel k = clCreateKernel(pr, "saxpy", &e);
  cl_mem bx = clCreateBuffer(ctx, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, N*4, x, &e);
  cl_mem by = clCreateBuffer(ctx, CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR, N*4, y, &e);
  clSetKernelArg(k,0,sizeof bx,&bx); clSetKernelArg(k,1,sizeof by,&by);
  clSetKernelArg(k,2,sizeof A,&A); clSetKernelArg(k,3,sizeof IT,&IT);
  size_t g = N; clFinish(q);
  double g0 = now(); e = clEnqueueNDRangeKernel(q,k,1,NULL,&g,NULL,0,NULL,NULL); clFinish(q); double gpu = now() - g0;
  clEnqueueReadBuffer(q, by, CL_TRUE, 0, N*4, y, 0, NULL, NULL);
  int bad = 0; for (int i = 0; i < N; i++) if (fabsf(y[i]-ref[i]) > 1e-3f*fabsf(ref[i])+1e-4f) bad++;
  printf("enqueue=%d N=%d iters=%d  mismatches=%d  gpu=%.1f ms  cpu(1 core)=%.1f ms  gpu GFLOPS=%.2f\n",
         e, N, IT, bad, gpu*1e3, cpu*1e3, 3.0*N*IT/gpu/1e9);
  return bad != 0;
}
