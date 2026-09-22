// Trace the transitions of the DSI video stream generator (cdns-dsi, 7c018000).
// Usage: dsi_trace <seconds> [period_us, default 200]
// CLOCK_MONOTONIC times, in seconds. NOT aligned with dmesg: a skew of at least
// 77 ms was measured, so only compare times within the same trace.
// Samples every period_us (default 200 us); 1000 is enough for long traces.
//
// With good video VSG_RUNNING drops to 0 for about 0.3 ms on every frame (the
// inter frame gap, non continuous clock). Therefore:
// - a change of MCTL_MAIN_DATA_CTL or VID_MODE_STS_FLAG is printed at once;
// - a change of VID_MODE_STS or MCTL_LANE_STS is printed only if it lasts at
//   least STABLE_S, otherwise it is counted as a pulse.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define BASE 0x7c018000UL
#define DATA_CTL 0x004
#define LANE_STS 0x02c
#define VID_STS 0x0f0
#define VID_FLAG 0x180
#define STABLE_S 0.002

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void show(double t, const char *why, uint32_t d, uint32_t v, uint32_t f, uint32_t l)
{
	printf("[%12.6f] %-6s VID_EN=%u VSG=%s STS=0x%03x FLAG=0x%03x LANE=0x%08x\n",
	       t, why, (d >> 5) & 1, (v & 1) ? "GIRA " : "FERMO", v, f, l);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	double dur = argc > 1 ? atof(argv[1]) : 20;
	long period_us = argc > 2 ? atol(argv[2]) : 200;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	uint32_t *r = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, BASE);
	if (r == MAP_FAILED) { perror("mmap"); return 1; }

	const struct timespec nap = { period_us / 1000000, (period_us % 1000000) * 1000 };
	double t0 = now_s(), end = t0 + dur;
	unsigned long samples = 0, blips = 0;

	// last state printed
	uint32_t pd = r[DATA_CTL / 4], pf = r[VID_FLAG / 4];
	uint32_t pv = r[VID_STS / 4], pl = r[LANE_STS / 4];
	// candidate for VID_STS/LANE_STS, with the instant it appeared
	uint32_t cv = pv, cl = pl;
	double ct = t0;

	printf("tracing for %.1f s, period %ld us\n", dur, period_us);
	show(t0, "inizio", pd, pv, pf, pl);
	for (double t = t0; t < end; t = now_s()) {
		uint32_t d = r[DATA_CTL / 4], v = r[VID_STS / 4];
		uint32_t f = r[VID_FLAG / 4], l = r[LANE_STS / 4];
		samples++;

		if (d != pd || f != pf) {
			show(t, d != pd ? "ctl" : "flag", d, v, f, l);
			pd = d; pf = f; pv = v; pl = l;
			cv = v; cl = l; ct = t;
		} else if (v != cv || l != cl) {
			// il candidato cambia: se il precedente non era durato abbastanza
			// and it differed from the printed state, so it was a pulse
			if ((cv != pv || cl != pl) && t - ct < STABLE_S)
				blips++;
			cv = v; cl = l; ct = t;
		} else if ((cv != pv || cl != pl) && t - ct >= STABLE_S) {
			show(ct, "stato", d, cv, f, cl);
			pv = cv; pl = cl;
		}
		nanosleep(&nap, NULL);
	}
	double el = now_s() - t0;
	printf("done: %lu samples in %.1f s (%.0f per second), short pulses ignored: %lu\n",
	       samples, el, samples / el, blips);
	return 0;
}
