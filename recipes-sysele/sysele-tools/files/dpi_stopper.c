// T12 Prova 2: ferma il DPI alla discesa di VID_EN, azzerando il bit 23 di DPM_1.
// Uso: dpi_stopper <modo: 1 ferma, 0 osserva soltanto> <timeout_s>
//
// Basi: DSI 0x7c018000, DPI 0x7c019000.
// Alla discesa di VID_EN (MCTL_MAIN_DATA_CTL bit 5), in modo 1 scrive subito
// DPM_1 (DPI+0x0e4) con CFG_IS_CONTINUOUS_SCANOUT_MODE (bit 23) azzerato.
// Poi, ogni 1 ms fino alla risalita, legge VID_MODE_STS (DSI+0x0f0). Non scrive
// mai VID_MODE_STS_CLR. Alla risalita rilegge DPM_1: se il bit 23 e' ancora
// zero lo rimette e riemette HA_STREAM_START (DPI+0x0f8) e GO_SCANOUT
// (DPI+0x0f4), segnalandolo. Infine legge STS e FLAG a +5, +20, +50 e +100 ms.
// Il modo 0 fa le stesse letture senza scrivere niente.
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

	printf("[%.3f] dpi_stopper modo %d (%s), VID_EN ora %d, STS 0x%03x FLAG 0x%03x, DPM_1 0x%08x\n",
	       t0, mode, mode ? "ferma il DPI" : "osserva", prev_en, dsi[VID_STS / 4],
	       dsi[VID_FLAG / 4], dpi[DPM_1 / 4]);
	fflush(stdout);

	double tf = -1;
	while (now_s() < end) {
		uint32_t d = dsi[DATA_CTL / 4], s = dsi[VID_STS / 4];
		int en = (d & VID_EN) != 0;
		if (en && (s & ERR_BITS) && !err_seen) {
			err_seen = 1;
			printf("[%.3f] ATTENZIONE errore vivo con VID_EN alto prima della discesa: STS 0x%03x (uscita da scartare)\n",
			       now_s(), s);
			fflush(stdout);
		}
		if (prev_en && !en) { tf = now_s(); break; }
		prev_en = en;
		nanosleep(&poll, NULL);
	}
	if (tf < 0) { printf("[%.3f] nessuna discesa entro il timeout\n", now_s()); return 2; }

	uint32_t dpm1_prima = dpi[DPM_1 / 4];
	double tw = -1;
	if (mode) {
		dpi[DPM_1 / 4] = dpm1_prima & ~CONT_SCANOUT;
		tw = now_s();
	}
	printf("[%.3f] DISCESA di VID_EN: STS 0x%03x FLAG 0x%03x DPM_1 0x%08x%s\n", tf,
	       dsi[VID_STS / 4], dsi[VID_FLAG / 4], dpm1_prima, mode ? "" : " (nessuna scrittura)");
	if (mode)
		printf("         DPI fermato a +%.3f ms dalla discesa, DPM_1 ora 0x%08x\n",
		       (tw - tf) * 1e3, dpi[DPM_1 / 4]);
	fflush(stdout);

	int n = 0, k = 0;
	double tr = -1, t_last_err = -1;
	for (;;) {
		double t = now_s();
		if (dsi[DATA_CTL / 4] & VID_EN) { tr = t; break; }
		if (t - tf > 30.0) { printf("[%.3f] VID_EN ancora basso dopo 30 s: esco\n", t); return 3; }
		uint32_t s = dsi[VID_STS / 4];
		if (s & ERR_BITS)
			t_last_err = t;
		if (n < MAXS) { ts_t[n] = t; ts_sts[n] = s; n++; }
		k++;
		sleep_until(tf + k * 0.001);
	}

	uint32_t dpm1_risalita = dpi[DPM_1 / 4];
	int rimesso = 0;
	if (!(dpm1_risalita & CONT_SCANOUT)) {
		dpi[DPM_1 / 4] = dpm1_risalita | CONT_SCANOUT;
		dpi[HA_STREAM_START / 4] = 1;
		dpi[GO_SCANOUT / 4] = 1;
		rimesso = 1;
	}
	printf("[%.3f] RISALITA di VID_EN dopo %.1f ms; DPM_1 alla risalita 0x%08x%s\n", tr,
	       (tr - tf) * 1e3, dpm1_risalita,
	       rimesso ? "  ATTENZIONE bit 23 ancora zero: rimesso io, con HA_STREAM_START e GO_SCANOUT" : "");

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
	printf("         letture a VID_EN basso: %d, con errore vivo: %d", n, miss);
	if (t_last_err > 0)
		printf(", ultimo errore a +%.1f ms dalla discesa", (t_last_err - tf) * 1e3);
	printf("; valori:");
	for (int j = 0; j < uniq; j++)
		printf(" 0x%03x x%d", val[j], cnt[j]);
	printf("\n         prime letture (ms dalla discesa, STS):");
	for (int i = 0; i < n && i < 6; i++)
		printf(" %.1f=0x%03x", (ts_t[i] - tf) * 1e3, ts_sts[i]);
	printf("\n");
	for (int i = 0; i < 4; i++)
		printf("         +%3.0f ms: STS 0x%03x FLAG 0x%03x\n", off[i] * 1e3, ps[i], pf[i]);
	printf("[%.3f] ESITO a +50 ms: %s (STS 0x%03x)%s\n", now_s(),
	       ps[2] == 0x001 ? "SANO" : "FERMO", ps[2],
	       err_seen ? " [SCARTARE: errore prima della discesa]" : "");
	return 0;
}
