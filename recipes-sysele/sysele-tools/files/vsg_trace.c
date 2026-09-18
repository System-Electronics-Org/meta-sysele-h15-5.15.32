// Traccia le transizioni del generatore video DSI (cdns-dsi, 7c018000).
// Uso: vsg_trace <secondi> [periodo_us, default 200]
// Tempi in CLOCK_MONOTONIC (s). NON allineati con il dmesg: misurato uno
// scarto di almeno 77 ms, confrontare solo tempi della stessa traccia.
// Campiona ogni periodo_us (default 200 us); per tracce lunghe basta 1000.
//
// Con il video buono VSG_RUNNING cade a 0 per circa 0,3 ms a ogni frame
// (intervallo fra frame, clock non continuo). Quindi:
// - un cambio di MCTL_MAIN_DATA_CTL o di VID_MODE_STS_FLAG si stampa subito;
// - un cambio di VID_MODE_STS o MCTL_LANE_STS si stampa solo se dura almeno
//   STABLE_S, altrimenti si conta come impulso.
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

	// ultimo stato stampato
	uint32_t pd = r[DATA_CTL / 4], pf = r[VID_FLAG / 4];
	uint32_t pv = r[VID_STS / 4], pl = r[LANE_STS / 4];
	// candidato per VID_STS/LANE_STS, con l'istante in cui e' comparso
	uint32_t cv = pv, cl = pl;
	double ct = t0;

	printf("traccia per %.1f s, periodo %ld us\n", dur, period_us);
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
			// ed era diverso dallo stato stampato, era un impulso
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
	printf("fine: %lu campioni in %.1f s (%.0f al secondo), impulsi brevi ignorati: %lu\n",
	       samples, el, samples / el, blips);
	return 0;
}
