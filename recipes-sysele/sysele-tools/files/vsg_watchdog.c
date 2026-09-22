// Userspace watchdog for the DSI video stream generator (cdns-dsi, 7c018000).
// A prototype of what now lives in the driver, useful to try it without a build.
// Usage: vsg_watchdog <seconds> [period_ms, default 1000] [pause_ms, default 100]
//
// Every period it reads VID_MODE_STS (0xf0). If VID_EN is set and either
// ERR_MISSING_DATA (bit 1) or ERR_SMALL_HEIGHT (bit 5) is live, it reads again
// after 50 ms. If they are still set it steps in: it clears VID_MODE_STS_CLR
// (0x160), drops VID_EN, waits pause_ms and raises it again.
// It does not use VSG_RUNNING on its own: with good video that drops to 0 for
// 0.3 ms on every frame. The live errors cover every stuck state measured so
// far: 0x020 (RECOVERY_MODE 0), 0x002 (mode 2), 0x403 (mode 3). A clean stop
// (0x000) does not trigger it.
// Prints every intervention with the CLOCK_MONOTONIC time and the gap from the
// previous one, plus the state 1 s later.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#define BASE 0x7c018000UL
#define DATA_CTL 0x004
#define VID_STS 0x0f0
#define VID_CLR 0x160
#define VID_FLAG 0x180
#define VID_EN (1u << 5)
#define ERR_BITS ((1u << 1) | (1u << 5))

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void nap_ms(long ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&t, NULL);
}

int main(int argc, char **argv)
{
	double dur = argc > 1 ? atof(argv[1]) : 3600;
	long period = argc > 2 ? atol(argv[2]) : 1000;
	long pause = argc > 3 ? atol(argv[3]) : 100;
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	uint32_t *r = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, BASE);
	if (r == MAP_FAILED) { perror("mmap"); return 1; }

	double t0 = now_s(), end = t0 + dur, last = -1;
	unsigned long count = 0;

	printf("[%12.3f] watchdog for %.0f s, period %ld ms, pause %ld ms\n", t0, dur, period, pause);
	fflush(stdout);
	while (now_s() < end) {
		uint32_t d = r[DATA_CTL / 4], s = r[VID_STS / 4];
		if ((d & VID_EN) && (s & ERR_BITS)) {
			nap_ms(50);
			d = r[DATA_CTL / 4];
			uint32_t s2 = r[VID_STS / 4];
			if ((d & VID_EN) && (s2 & ERR_BITS)) {
				double t = now_s();
				uint32_t f = r[VID_FLAG / 4];
				r[VID_CLR / 4] = 0x7ff;
				r[DATA_CTL / 4] = d & ~VID_EN;
				nap_ms(pause);
				r[DATA_CTL / 4] = d | VID_EN;
				count++;
				printf("[%12.3f] intervento %lu: STS 0x%03x FLAG 0x%03x, dal precedente %s",
				       t, count, s2, f, last < 0 ? "-" : "");
				if (last >= 0)
					printf("%.2f s", t - last);
				printf("\n");
				fflush(stdout);
				last = t;
				nap_ms(1000);
				printf("[%12.3f]   after 1 s: STS 0x%03x FLAG 0x%03x\n",
				       now_s(), r[VID_STS / 4], r[VID_FLAG / 4]);
				fflush(stdout);
				continue;
			}
		}
		nap_ms(period);
	}
	printf("[%12.3f] fine, interventi %lu\n", now_s(), count);
	return 0;
}
