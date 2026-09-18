// Watchdog del generatore video DSI (cdns-dsi, 7c018000) in spazio utente:
// prototipo del punto 3.3 del T09, per provarlo senza build.
// Uso: vsg_watchdog <secondi> [periodo_ms, default 1000] [pausa_ms, default 100]
//
// Ogni periodo legge VID_MODE_STS (0xf0). Se VID_EN e' acceso e sono accesi,
// vivi, ERR_MISSING_DATA (bit 1) o ERR_SMALL_HEIGHT (bit 5), rilegge dopo
// 50 ms. Se sono ancora accesi interviene: azzera VID_MODE_STS_CLR (0x160),
// abbassa VID_EN, attende pausa_ms, lo rialza.
// Non usa VSG_RUNNING da solo: con il video buono cade a 0 per 0,3 ms a ogni
// frame. Gli errori vivi coprono tutti gli stati bloccati misurati: 0x020
// (RECOVERY_MODE 0), 0x002 (modo 2), 0x403 (modo 3). Uno stop pulito (0x000)
// non fa intervenire.
// Stampa ogni intervento con il tempo CLOCK_MONOTONIC e l'intervallo dal
// precedente, e lo stato 1 s dopo.
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

	printf("[%12.3f] watchdog per %.0f s, periodo %ld ms, pausa %ld ms\n", t0, dur, period, pause);
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
				printf("[%12.3f]   dopo 1 s: STS 0x%03x FLAG 0x%03x\n",
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
