// Misura il periodo di frame del generatore video DSI (cdns-dsi, 7c018000)
// dagli impulsi di fine frame: VSG_RUNNING (VID_MODE_STS bit 0) cade a 0 per
// circa 0,3 ms a ogni frame. Campionamento continuo, senza pause.
// Uso: vsg_period <secondi>
// Stampa il periodo stimato con una retta ai minimi quadrati sui fronti di
// discesa, e lo confronta con il periodo del DPI (600 MHz / 10 000 000 cicli)
// e con quello del modo del pannello (1496 x 928 su 83 333 kHz).
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#define BASE 0x7c018000UL
#define VID_STS 0x0f0
#define DPI_PERIOD 1.0 / 60.0
#define MODE_PERIOD (1496.0 * 928.0) / 83333000.0

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	double dur = argc > 1 ? atof(argv[1]) : 60;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	uint32_t *r = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, BASE);
	if (r == MAP_FAILED) { perror("mmap"); return 1; }

	double t0 = now_s(), end = t0 + dur, first = 0, t_prev = 0;
	double sk = 0, st = 0, skk = 0, skt = 0;
	long n = 0, k = 0, dup = 0, samples = 0;
	int prev = r[VID_STS / 4] & 1;

	if (!prev) {
		printf("VSG fermo: niente da misurare, lancia prima vsg.sh fix\n");
		return 1;
	}
	for (double t = t0; t < end; t = now_s()) {
		int v = r[VID_STS / 4] & 1;
		samples++;
		if (prev && !v) {
			if (n == 0) {
				first = t;
				k = 0;
			} else {
				// indice del frame dall'intervallo col fronte precedente: un
				// errore di fase accumulato non fa sbagliare il conteggio
				long step = lround((t - t_prev) / (DPI_PERIOD));
				if (step == 0) {
					dup++;
					prev = v;
					continue;
				}
				k += step;
			}
			sk += k; st += t - first; skk += (double)k * k; skt += k * (t - first);
			n++;
			t_prev = t;
		}
		prev = v;
	}

	if (n < 10) {
		printf("solo %ld fronti in %.1f s: il VSG si e' fermato?\n", n, dur);
		return 1;
	}
	double slope = (n * skt - sk * st) / (n * skk - sk * sk);
	long prev_k = k;

	printf("campioni %ld in %.1f s, fronti %ld, frame coperti %ld, fronti doppi %ld\n",
	       samples, now_s() - t0, n, prev_k + 1, dup);
	printf("periodo misurato  %.6f ms  (%.5f Hz)\n", slope * 1e3, 1.0 / slope);
	printf("periodo DPI       %.6f ms  (60.00000 Hz)   scarto %+.1f ppm\n",
	       (DPI_PERIOD) * 1e3, (slope / (DPI_PERIOD) - 1) * 1e6);
	printf("periodo del modo  %.6f ms  (%.5f Hz)   scarto %+.1f ppm\n",
	       (MODE_PERIOD) * 1e3, 1.0 / (MODE_PERIOD), (slope / (MODE_PERIOD) - 1) * 1e6);
	return 0;
}
