/* ts_scan.c - 扫一页寄存器，找出在变化的（时间戳计数器） */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <stdint.h>
int main(int argc, char **argv){
	if(argc<2){printf("usage: ts_scan <addr> [nwords] [ms]\n");return 1;}
	uint64_t addr=strtoull(argv[1],0,0);
	int n=argc>2?atoi(argv[2]):256;
	int ms=argc>3?atoi(argv[3]):1000;
	int fd=open("/dev/mem",O_RDWR|O_SYNC);
	if(fd<0){perror("open /dev/mem");return 1;}
	volatile uint32_t *p=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,(off_t)addr);
	if(p==MAP_FAILED){perror("mmap");return 1;}
	uint32_t a[512];
	for(int i=0;i<n;i++) a[i]=p[i];
	struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
	usleep(ms*1000);
	clock_gettime(CLOCK_MONOTONIC,&t1);
	double dt=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;
	printf("addr=0x%llX n=%d dt=%.6f s\n",(unsigned long long)addr,n,dt);
	int found=0;
	for(int i=0;i<n;i++){
		uint32_t v=p[i];
		if(v!=a[i]){ found++;
			printf("  +0x%03X: 0x%08X -> 0x%08X  d=%u  ~%.3f MHz\n",
				i*4,a[i],v,(uint32_t)(v-a[i]),(double)((uint32_t)(v-a[i]))/dt/1e6);
		}
	}
	if(!found) printf("  (没有任何寄存器在变)\n");
	return 0;
}
