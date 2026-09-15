/* experiment: 3A helper that calls ispSetFpsRanage(fps) after start; runs until SIGTERM */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <AWIspApi.h>
static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }
int main(int argc, char **argv)
{
	int fps = argc > 1 ? atoi(argv[1]) : 0;
	AWIspApi *isp = CreateAWIspApi();
	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
	if (!isp || isp->ispApiInit() < 0) return 1;
	int id = isp->ispGetIspId(0);
	if (id < 0 || isp->ispStart(id) < 0) { fprintf(stderr, "start failed\n"); return 1; }
	if (fps > 0) printf("ispSetFpsRanage(%d) = %d\n", fps, isp->ispSetFpsRanage(id, fps));
	printf("ready isp%d\n", id); fflush(stdout);
	while (!stop) pause();
	isp->ispStop(id); isp->ispWaitToExit(id); isp->ispApiUnInit(); DestroyAWIspApi(isp);
	return 0;
}
