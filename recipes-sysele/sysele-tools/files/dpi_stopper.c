// Stop the DPI on the falling edge of VID_EN by clearing bit 23 of DPM_1, to
// establish whether the generator restarts clean once the DPI stops scanning.
// Usage: dpi_stopper <mode: 1 stop, 0 observe only> <timeout_s>
//
// Bases: DSI 0x7c018000, DPI 0x7c019000.
// On the falling edge of VID_EN (MCTL_MAIN_DATA_CTL bit 5), mode 1 writes DPM_1
// (DPI+0x0e4) straight away with CFG_IS_CONTINUOUS_SCANOUT_MODE (bit 23) cleared.
// Then, every 1 ms until the rising edge, it reads VID_MODE_STS (DSI+0x0f0). It
// never writes VID_MODE_STS_CLR. On the rising edge it reads DPM_1 back: if bit
// 23 is still zero it restores it and reissues HA_STREAM_START (DPI+0x0f8) and
// GO_SCANOUT (DPI+0x0f4), reporting it. Finally it reads STS and FLAG at +5,
// +20, +50 and +100 ms. Mode 0 does the same reads without writing anything.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#define DSI_BASE 0x7c018000UL
#define DPI_BASE 0x7c019000UL
#define DATA_CTL 0x004
#define VID_STS 0x0f0
#define VID_FLAG 0x180
#define DPM_1 0x0e4
#define GO_SCANOUT 0x0f4
#define HA_STREAM_START 0x0f8
#define CONT_SCANOUT (1u << 23)
#define VID_EN (1u << 5)
#define ERR_BITS ((1u << 1) | (1u << 5))
#define MAXS 20000

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void sleep_until(double target)
{
	for (;;) {
		double d = target - now_s();
		if (d <= 0)
			return;
		if (d > 0.0003) {
			struct timespec ts = { 0, (long)((d - 0.0002) * 1e9) };
			nanosleep(&ts, NULL);
		}
	}
}

static uint32_t ts_sts[MAXS];
static double ts_t[MAXS];

int main(int argc, char **argv)
{
	int mode = argc > 1 ? atoi(argv[1]) : 1;
	double tmo = argc > 2 ? atof(argv[2]) : 120;
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	uint32_t *dsi = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, DSI_BASE);
	uint32_t *dpi = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, DPI_BASE);
	if (dsi == MAP_FAILED || dpi == MAP_FAILED) { perror("mmap"); return 1; }

	const struct timespec poll = { 0, 100000 };
	double t0 = now_s(), end = t0 + tmo;
	int prev_en = (dsi[DATA_CTL / 4] & VID_EN) != 0;
	int err_seen = 0;

	printf("[%.3f] dpi_stopper mode %d (%s), VID_EN now %d, STS 0x%03x FLAG 0x%03x, DPM_1 0x%08x\n",
	       t0, mode, mode ? "stop the DPI" : "observe", prev_en, dsi[VID_STS / 4],
	       dsi[VID_FLAG / 4], dpi[DPM_1 / 4]);
	fflush(stdout);

	double tf = -1;
	while (now_s() < end) {
		uint32_t d = dsi[DATA_CTL / 4], s = dsi[VID_STS / 4];
		int en = (d & VID_EN) != 0;
		if (en && (s & ERR_BITS) && !err_seen) {
			err_seen = 1;
			printf("[%.3f] WARNING live error with VID_EN high before the falling edge: STS 0x%03x (discard this run)\n",
			       now_s(), s);
			fflush(stdout);
		}
		if (prev_en && !en) { tf = now_s(); break; }
		prev_en = en;
		nanosleep(&poll, NULL);
	}
	if (tf < 0) { printf("[%.3f] no falling edge within the timeout\n", now_s()); return 2; }

	uint32_t dpm1_before = dpi[DPM_1 / 4];
	double tw = -1;
	if (mode) {
		dpi[DPM_1 / 4] = dpm1_before & ~CONT_SCANOUT;
		tw = now_s();
	}
	printf("[%.3f] VID_EN FELL: STS 0x%03x FLAG 0x%03x DPM_1 0x%08x%s\n", tf,
	       dsi[VID_STS / 4], dsi[VID_FLAG / 4], dpm1_before, mode ? "" : " (no write)");
	if (mode)
		printf("         DPI stopped at +%.3f ms from the falling edge, DPM_1 now 0x%08x\n",
		       (tw - tf) * 1e3, dpi[DPM_1 / 4]);
	fflush(stdout);

	int n = 0, k = 0;
	double tr = -1, t_last_err = -1;
	for (;;) {
		double t = now_s();
		if (dsi[DATA_CTL / 4] & VID_EN) { tr = t; break; }
		if (t - tf > 30.0) { printf("[%.3f] VID_EN still low after 30 s: giving up\n", t); return 3; }
		uint32_t s = dsi[VID_STS / 4];
		if (s & ERR_BITS)
			t_last_err = t;
		if (n < MAXS) { ts_t[n] = t; ts_sts[n] = s; n++; }
		k++;
		sleep_until(tf + k * 0.001);
	}

	uint32_t dpm1_rise = dpi[DPM_1 / 4];
	int restored = 0;
	if (!(dpm1_rise & CONT_SCANOUT)) {
		dpi[DPM_1 / 4] = dpm1_rise | CONT_SCANOUT;
		dpi[HA_STREAM_START / 4] = 1;
		dpi[GO_SCANOUT / 4] = 1;
		restored = 1;
	}
	printf("[%.3f] VID_EN ROSE after %.1f ms; DPM_1 on the rising edge 0x%08x%s\n", tr,
	       (tr - tf) * 1e3, dpm1_rise,
	       restored ? "  WARNING bit 23 still zero: restored here, with HA_STREAM_START and GO_SCANOUT" : "");

	const double off[4] = { 0.005, 0.020, 0.050, 0.100 };
	uint32_t ps[4], pf[4];
	for (int i = 0; i < 4; i++) {
		sleep_until(tr + off[i]);
		ps[i] = dsi[VID_STS / 4];
		pf[i] = dsi[VID_FLAG / 4];
	}

	int miss = 0, uniq = 0;
	uint32_t val[16];
	int cnt[16];
	for (int i = 0; i < n; i++) {
		if (ts_sts[i] & ERR_BITS)
			miss++;
		int j;
		for (j = 0; j < uniq; j++)
			if (val[j] == ts_sts[i]) { cnt[j]++; break; }
		if (j == uniq && uniq < 16) { val[uniq] = ts_sts[i]; cnt[uniq] = 1; uniq++; }
	}
	printf("         reads with VID_EN low: %d, with a live error: %d", n, miss);
	if (t_last_err > 0)
		printf(", last error at +%.1f ms from the falling edge", (t_last_err - tf) * 1e3);
	printf("; values:");
	for (int j = 0; j < uniq; j++)
		printf(" 0x%03x x%d", val[j], cnt[j]);
	printf("\n         first reads (ms from the falling edge, STS):");
	for (int i = 0; i < n && i < 6; i++)
		printf(" %.1f=0x%03x", (ts_t[i] - tf) * 1e3, ts_sts[i]);
	printf("\n");
	for (int i = 0; i < 4; i++)
		printf("         +%3.0f ms: STS 0x%03x FLAG 0x%03x\n", off[i] * 1e3, ps[i], pf[i]);
	printf("[%.3f] OUTCOME at +50 ms: %s (STS 0x%03x)%s\n", now_s(),
	       ps[2] == 0x001 ? "HEALTHY" : "STOPPED", ps[2],
	       err_seen ? " [DISCARD: error before the falling edge]" : "");
	return 0;
}
