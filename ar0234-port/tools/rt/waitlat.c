// waitlat <cpu> <mode: sleep|spin|hybrid> <seconds> [guard_us]
// Every 200 us wake at an absolute deadline and record how late we are.
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
static inline int64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000000LL+t.tv_nsec;}
int main(int c, char **v){
  int cpu = atoi(v[1]); const char *mode = v[2]; int secs = atoi(v[3]);
  int64_t guard = (c > 4 ? atoi(v[4]) : 60) * 1000LL, period = 200000;
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s); sched_setaffinity(0, sizeof s, &s);
  struct sched_param p = { .sched_priority = 95 }; sched_setscheduler(0, SCHED_FIFO, &p);
  mlockall(MCL_CURRENT|MCL_FUTURE);
  int fd = open("/dev/cpu_dma_latency", O_WRONLY); int32_t z = 0; if (fd >= 0) write(fd, &z, 4);
  int64_t hist[1001] = {0}, max = 0, sum = 0, n = 0, next = ns() + period, end = ns() + secs*1000000000LL;
  while (next < end) {
    if (!strcmp(mode, "sleep") || !strcmp(mode, "hybrid")) {
      int64_t w = !strcmp(mode, "sleep") ? next : next - guard;
      struct timespec t = { w / 1000000000LL, w % 1000000000LL };
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
    }
    int64_t now; while ((now = ns()) < next) ;   /* spin to the deadline (no-op for sleep mode) */
    int64_t late = now - next; if (late > max) max = late; sum += late; n++;
    int us = late / 1000; hist[us > 1000 ? 1000 : us]++;
    next += period;
  }
  int64_t p999 = 0, p99999 = 0, acc = 0;
  for (int i = 0; i <= 1000; i++) { acc += hist[i]; if (!p999 && acc >= n*0.999) p999 = i; if (!p99999 && acc >= n*0.99999) p99999 = i; }
  printf("%-6s cpu%d n=%lld avg=%.2fus p99.9=%lldus p99.999=%lldus max=%.2fus\n", mode, cpu, (long long)n, sum/1e3/n, (long long)p999, (long long)p99999, max/1e3);
  return 0;
}
