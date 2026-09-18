// Azzera VID_MODE_STS_CLR di continuo mentre VID_EN e' basso, per stabilire se
// sono i flag vecchi a impedire al generatore di ripartire.
// Uso: clr_keeper <modo: 1 azzera, 0 osserva soltanto> <timeout_s>
//
// Base DSI 0x7c018000. Attende una discesa di VID_EN (MCTL_MAIN_DATA_CTL bit 5).
// Mentre VID_EN e' basso, ogni 1 ms legge VID_MODE_STS (+0x0f0) e, in modo 1,
// subito dopo scrive VID_MODE_STS_CLR (+0x160) = 0x7FF. Alla risalita, in modo
// 1, fa un ultimo azzeramento immediato. Poi legge VID_MODE_STS e
// VID_MODE_STS_FLAG (+0x180) a +5, +20, +50 e +100 ms dalla risalita, stampa
// ed esce. In modo 0 fa le stesse letture senza scrivere niente.
// Mentre attende la discesa segnala errori vivi con VID_EN alto (un intoppo
// dei 587 s durante lo stream: l'uscita va scartata).
// Tempi CLOCK_MONOTONIC in secondi.
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

	printf("[%.3f] clr_keeper modo %d (%s), attesa discesa di VID_EN, VID_EN ora %d, STS 0x%03x FLAG 0x%03x\n",
	       t0, mode, mode ? "azzera" : "osserva", prev_en, r[VID_STS / 4], r[VID_FLAG / 4]);
	fflush(stdout);

	/* attesa della discesa */
	double tf = -1;
	while (now_s() < end) {
		uint32_t d = r[DATA_CTL / 4], s = r[VID_STS / 4];
		int en = (d & VID_EN) != 0;
		if (en && (s & ERR_BITS) && !err_seen) {
			err_seen = 1;
			printf("[%.3f] ATTENZIONE errore vivo con VID_EN alto prima della discesa: STS 0x%03x FLAG 0x%03x (uscita da scartare)\n",
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
		printf("[%.3f] nessuna discesa entro il timeout\n", now_s());
		return 2;
	}
	printf("[%.3f] DISCESA di VID_EN: STS 0x%03x FLAG 0x%03x\n", tf, r[VID_STS / 4], r[VID_FLAG / 4]);
	fflush(stdout);

	/* VID_EN basso: lettura ogni 1 ms, azzeramento in modo 1 */
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
			printf("[%.3f] VID_EN ancora basso dopo 30 s: esco\n", t);
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
	printf("[%.3f] RISALITA di VID_EN dopo %.1f ms%s\n", tr, (tr - tf) * 1e3,
	       mode ? "; ultimo azzeramento eseguito" : "");
	if (mode)
		printf("         azzeramento finale %.3f ms dopo la risalita\n", (tc - tr) * 1e3);

	/* letture dopo la risalita */
	const double off[4] = { 0.005, 0.020, 0.050, 0.100 };
	uint32_t ps[4], pf[4];
	double pt[4];
	for (int i = 0; i < 4; i++) {
		sleep_until(tr + off[i]);
		pt[i] = now_s();
		ps[i] = r[VID_STS / 4];
		pf[i] = r[VID_FLAG / 4];
	}

	/* riassunto delle letture a VID_EN basso */
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
	printf("         letture a VID_EN basso: %d, con ERR_MISSING_DATA (bit 1): %d; valori:", n, miss);
	for (int j = 0; j < uniq; j++)
		printf(" 0x%03x x%d", val[j], cnt[j]);
	printf("\n         prime letture (ms dalla discesa, STS):");
	for (int i = 0; i < n && i < 6; i++)
		printf(" %.1f=0x%03x", (ts_t[i] - tf) * 1e3, ts_sts[i]);
	printf("\n         ultime letture:");
	for (int i = n > 3 ? n - 3 : 0; i < n; i++)
		printf(" %.1f=0x%03x", (ts_t[i] - tf) * 1e3, ts_sts[i]);
	printf("\n");
	for (int i = 0; i < 4; i++)
		printf("         +%3.0f ms (%.1f): STS 0x%03x FLAG 0x%03x\n", off[i] * 1e3,
		       (pt[i] - tr) * 1e3, ps[i], pf[i]);
	printf("[%.3f] ESITO a +50 ms: %s (STS 0x%03x)%s\n", now_s(),
	       ps[2] == 0x001 ? "SANO" : "FERMO", ps[2], err_seen ? " [SCARTARE: errore prima della discesa]" : "");
	return 0;
}
