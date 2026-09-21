// Keep clearing VID_MODE_STS_CLR while VID_EN is low, to establish whether it is
// the stale flags that stop the generator from restarting.
// Usage: clr_keeper <mode: 1 clear, 0 observe only> <timeout_s>
//
// DSI base 0x7c018000. Waits for a falling edge of VID_EN (MCTL_MAIN_DATA_CTL
// bit 5). While VID_EN is low it reads VID_MODE_STS (+0x0f0) every 1 ms and, in
// mode 1, writes VID_MODE_STS_CLR (+0x160) = 0x7FF right after. On the rising
// edge, in mode 1, it does one last immediate clear. Then it reads VID_MODE_STS
// and VID_MODE_STS_FLAG (+0x180) at +5, +20, +50 and +100 ms from the rising
// edge, prints and exits. Mode 0 does the same reads without writing anything.
// While waiting for the falling edge it reports live errors with VID_EN high (a
// stall at 587 s during the stream: that run must be discarded).
// CLOCK_MONOTONIC times, in seconds.
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
	uint32_t *r = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, BASE);
	if (r == MAP_FAILED) { perror("mmap"); return 1; }

	const struct timespec poll = { 0, 100000 };
	double t0 = now_s(), end = t0 + tmo;
	int prev_en = (r[DATA_CTL / 4] & VID_EN) != 0;
	int err_seen = 0;

	printf("[%.3f] clr_keeper mode %d (%s), waiting for VID_EN to fall, VID_EN now %d, STS 0x%03x FLAG 0x%03x\n",
	       t0, mode, mode ? "clear" : "observe", prev_en, r[VID_STS / 4], r[VID_FLAG / 4]);
	fflush(stdout);

	/* wait for the falling edge */
	double tf = -1;
	while (now_s() < end) {
		uint32_t d = r[DATA_CTL / 4], s = r[VID_STS / 4];
		int en = (d & VID_EN) != 0;
		if (en && (s & ERR_BITS) && !err_seen) {
			err_seen = 1;
			printf("[%.3f] WARNING live error with VID_EN high before the falling edge: STS 0x%03x FLAG 0x%03x (discard this run)\n",
			       now_s(), s, r[VID_FLAG / 4]);
			fflush(stdout);
		}
		if (prev_en && !en) {
			tf = now_s();
			break;
		}
		prev_en = en;
		nanosleep(&poll, NULL);
	}
	if (tf < 0) {
		printf("[%.3f] no falling edge within the timeout\n", now_s());
		return 2;
	}
	printf("[%.3f] DISCESA di VID_EN: STS 0x%03x FLAG 0x%03x\n", tf, r[VID_STS / 4], r[VID_FLAG / 4]);
	fflush(stdout);

	/* VID_EN low: read every 1 ms, clear in mode 1 */
	int n = 0, k = 0;
	double tr = -1;
	for (;;) {
		double t = now_s();
		uint32_t d = r[DATA_CTL / 4];
		if (d & VID_EN) {
			tr = t;
			break;
		}
		if (t - tf > 30.0) {
			printf("[%.3f] VID_EN still low after 30 s: giving up\n", t);
			return 3;
		}
		uint32_t s = r[VID_STS / 4];
		if (n < MAXS) {
			ts_t[n] = t;
			ts_sts[n] = s;
			n++;
		}
		if (mode)
			r[VID_CLR / 4] = 0x7ff;
		k++;
		sleep_until(tf + k * 0.001);
	}
	if (mode)
		r[VID_CLR / 4] = 0x7ff;
	double tc = now_s();
	printf("[%.3f] VID_EN ROSE after %.1f ms%s\n", tr, (tr - tf) * 1e3,
	       mode ? "; last clear done" : "");
	if (mode)
		printf("         final clear %.3f ms after the rising edge\n", (tc - tr) * 1e3);

	/* reads after the rising edge */
	const double off[4] = { 0.005, 0.020, 0.050, 0.100 };
	uint32_t ps[4], pf[4];
	double pt[4];
	for (int i = 0; i < 4; i++) {
		sleep_until(tr + off[i]);
		pt[i] = now_s();
		ps[i] = r[VID_STS / 4];
		pf[i] = r[VID_FLAG / 4];
	}

	/* summary of the reads taken with VID_EN low */
	int miss = 0, uniq = 0;
	uint32_t val[16];
	int cnt[16];
	for (int i = 0; i < n; i++) {
		if (ts_sts[i] & (1u << 1))
			miss++;
		int j;
		for (j = 0; j < uniq; j++)
			if (val[j] == ts_sts[i]) { cnt[j]++; break; }
		if (j == uniq && uniq < 16) { val[uniq] = ts_sts[i]; cnt[uniq] = 1; uniq++; }
	}
	printf("         reads with VID_EN low: %d, with ERR_MISSING_DATA (bit 1): %d; values:", n, miss);
	for (int j = 0; j < uniq; j++)
		printf(" 0x%03x x%d", val[j], cnt[j]);
	printf("\n         first reads (ms from the falling edge, STS):");
	for (int i = 0; i < n && i < 6; i++)
		printf(" %.1f=0x%03x", (ts_t[i] - tf) * 1e3, ts_sts[i]);
	printf("\n         last reads:");
	for (int i = n > 3 ? n - 3 : 0; i < n; i++)
		printf(" %.1f=0x%03x", (ts_t[i] - tf) * 1e3, ts_sts[i]);
	printf("\n");
	for (int i = 0; i < 4; i++)
		printf("         +%3.0f ms (%.1f): STS 0x%03x FLAG 0x%03x\n", off[i] * 1e3,
		       (pt[i] - tr) * 1e3, ps[i], pf[i]);
	printf("[%.3f] ESITO a +50 ms: %s (STS 0x%03x)%s\n", now_s(),
	       ps[2] == 0x001 ? "HEALTHY" : "STOPPED", ps[2], err_seen ? " [DISCARD: error before the falling edge]" : "");
	return 0;
}
