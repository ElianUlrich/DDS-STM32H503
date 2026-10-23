/**
 * test_osc.c - Prueba del núcleo del osciloscopio en la PC (sin hardware).
 *
 *   make test
 *
 * Simula el ADC de 12 bits con ruido, verifica la base de tiempo, el trigger,
 * la medición de frecuencia y la reconstrucción en tiempo equivalente (ETS).
 * Escribe doc/datos_ets.dat y doc/datos_ets_rt.dat para el PDF, y
 * build/trama_prueba.bin para probar el visor de PC sin placa.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "osc_core.h"

#define PI     3.14159265358979323846
#define TIMCLK 250000000UL
#define ADCCLK 31250000UL

static int s_fail;
#define CHECK(cond, ...) do { \
    printf("  [%s] ", (cond) ? " OK " : "FALLA"); printf(__VA_ARGS__); printf("\n"); \
    if (!(cond)) s_fail++; } while (0)

static uint16_t buf[OSC_N_CAPTURE];
static uint16_t out[OSC_W_ETS];

/* Ruido gaussiano aproximado (suma de 4 uniformes), desvío ~ sigma. */
static double noise(double sigma)
{
    double s = 0;
    for (int i = 0; i < 4; i++) s += (double)rand() / RAND_MAX - 0.5;
    return s * sigma * 1.732;
}

/* "ADC" de 12 bits: seno de amplitud a [V] sobre 1,65 V, con ruido de 1 LSB,
 * ya normalizado a 16 bits como lo deja el firmware.                       */
static void capture_sine(double f, double fs, double a, double ph0)
{
    for (unsigned k = 0; k < OSC_N_CAPTURE; k++) {
        double v = 1.65 + a * sin(2 * PI * f * k / fs + ph0);
        double code = v / 3.3 * 4096.0 + noise(1.0);
        if (code < 0) code = 0;
        if (code > 4095) code = 4095;
        buf[k] = (uint16_t)((unsigned)(code + 0.5) << 4);
    }
}

/* Error RMS de FORMA (en LSB de 12 bits) de la reconstrucción ETS contra el
 * seno ideal. Se descuenta el mejor corrimiento de fase (que es sólo la
 * alineación del trigger) y se lo devuelve en casilleros.                  */
static double s_shift;
static double ets_error(double a)
{
    double best = 1e9;
    for (double d = -2.0; d <= 2.0; d += 0.01) {
        double s2 = 0;
        for (unsigned b = 0; b < OSC_ETS_BINS; b++) {
            double ph = (b + 0.5 + d) / OSC_ETS_BINS;
            double ideal = (1.65 + a * sin(2 * PI * ph)) / 3.3 * 65536.0;
            double e = (out[b] - ideal) / 16.0;
            s2 += e * e;
        }
        if (s2 < best) { best = s2; s_shift = d; }
    }
    return sqrt(best / OSC_ETS_BINS);
}

/* Simula el modo ETS completo, como lo hace osc.c. */
static void run_ets(double f, uint16_t *arr_used, uint32_t *cov, double *ferr, double *erms)
{
    uint16_t arr = 124;                           /* 2 MS/s */
    osc_meas_t m;
    for (int pass = 0; pass < 2; pass++) {
        double fs = (double)TIMCLK / (arr + 1);
        capture_sine(f, fs, 1.2, 0.3);
        osc_measure(buf, OSC_N_CAPTURE, &m);
        if (!m.period_valid) { *cov = 0; return; }
        uint16_t best = osc_ets_best_arr(m.period, arr, 124, 131, OSC_N_CAPTURE, cov);
        if (best == arr || pass == 1) break;
        arr = best;                               /* recaptura desafinada */
    }
    double fs = (double)TIMCLK / (arr + 1);
    *cov = osc_ets_reconstruct(buf, OSC_N_CAPTURE, m.period, m.t0, out);
    *arr_used = arr;
    *ferr = fabs(fs / m.period - f) / f * 1e6;    /* ppm */
    *erms = ets_error(1.2);
}

int main(int argc, char **argv)
{
    const char *docdir = (argc > 1) ? argv[1] : ".";
    const char *bindir = (argc > 2) ? argv[2] : ".";
    srand(1234);

    printf("1) Base de tiempo (ADC a 31,25 MHz, timer a 250 MHz)\n");
    printf("   Fs [S/s]   PSC   ARR  sobremuestreo  SMP  bits ef.\n");
    const uint32_t fss[] = { 2000000, 1000000, 500000, 200000, 100000, 50000,
                             20000, 10000, 5000, 2000, 1000 };
    static const float smp[8] = { 2.5f, 6.5f, 12.5f, 24.5f, 47.5f, 92.5f, 247.5f, 640.5f };
    for (unsigned i = 0; i < sizeof fss / sizeof fss[0]; i++) {
        osc_timebase_t tb;
        bool ok = osc_timebase_calc(fss[i], TIMCLK, ADCCLK, &tb);
        double conv_ns = (1u << tb.osr_log2) * (smp[tb.smp_code] + 12.5) / ADCCLK * 1e9;
        double per_ns  = 1e9 / tb.fs_hz;
        printf("   %8lu %5u %5u     x%-4u     %5.1f  %4.1f\n", (unsigned long)tb.fs_hz,
               tb.psc, tb.arr, 1u << tb.osr_log2, smp[tb.smp_code], tb.eff_bits_x10 / 10.0);
        if (!ok || tb.fs_hz != fss[i] || conv_ns > per_ns) {
            CHECK(0, "Fs = %lu: conversión %.0f ns > período %.0f ns", (unsigned long)fss[i], conv_ns, per_ns);
        }
    }
    CHECK(1, "todas las bases de tiempo: la conversión entra en el período de muestreo");

    printf("\n2) Medición de frecuencia en tiempo real\n");
    capture_sine(1000.0, 100000.0, 1.0, 0.0);
    osc_meas_t m;
    osc_measure(buf, OSC_N_CAPTURE, &m);
    double fm = 100000.0 / m.period;
    CHECK(m.period_valid && fabs(fm - 1000.0) < 0.5, "1 kHz a 100 kS/s -> %.3f Hz (%u cruces)", fm, m.ncross);
    CHECK(abs((int)m.vmax - (int)((1.65 + 1.0) / 3.3 * 65536)) < 400, "Vmax = %.3f V (ideal 2,650 V)", m.vmax * 3.3 / 65536);

    printf("\n3) Trigger\n");
    int32_t t = osc_find_trigger(buf, 256, OSC_N_CAPTURE, 32768, true);
    CHECK(t >= 256 && abs((int)buf[t] - 32768) < 3000, "flanco ascendente en 1,65 V -> muestra %d", t);
    int32_t tf = osc_find_trigger(buf, 256, OSC_N_CAPTURE, 32768, false);
    CHECK(tf > 0 && buf[tf - 1] > buf[tf], "flanco descendente -> muestra %d", tf);
    capture_sine(1000.0, 100000.0, 0.0, 0.0);
    CHECK(osc_find_trigger(buf, 256, OSC_N_CAPTURE, 32768, true) < 0, "señal continua: no dispara");

    printf("\n4) Tiempo equivalente (ETS) a ~2 MS/s con UN solo ADC\n");
    printf("   f_señal    muestras/periodo  ARR  cobertura  error f   forma RMS  alineación\n");
    const double fe[] = { 730000.0, 500000.0, 333333.0, 123456.0, 50000.0, 850000.0 };
    for (unsigned i = 0; i < sizeof fe / sizeof fe[0]; i++) {
        uint16_t arr; uint32_t cov; double ferr, erms;
        run_ets(fe[i], &arr, &cov, &ferr, &erms);
        double spp = (double)TIMCLK / (arr + 1) / fe[i];
        double sh_ns = s_shift / OSC_ETS_BINS / fe[i] * 1e9;
        printf("   %8.0f Hz     %6.2f        %3u    %3u/256   %6.1f ppm  %5.2f LSB  %+6.2f ns\n",
               fe[i], spp, arr, cov, ferr, erms, sh_ns);
        if (cov < 240 || erms > 2.0 || ferr > 200 || fabs(sh_ns) > 5.0)
            CHECK(0, "ETS a %.0f Hz fuera de tolerancia", fe[i]);
    }
    CHECK(1, "ETS: cobertura >= 240/256, forma < 2 LSB, frecuencia < 200 ppm y alineación < 5 ns");

    /* 500 kHz con Fs = 2 MS/s exactos: 4 muestras por período -> 4 fases. */
    double t4 = 4.0;
    CHECK(osc_ets_coverage((float)t4, OSC_N_CAPTURE) == 4, "sin desafinar, 500 kHz a 2 MS/s cubre solo 4 fases");
    uint32_t cov4;
    uint16_t a4 = osc_ets_best_arr((float)t4, 124, 124, 131, OSC_N_CAPTURE, &cov4);
    CHECK(a4 != 124 && cov4 > 240, "desafinando a ARR = %u (%.3f MS/s) cubre %u/256", a4, 250.0 / (a4 + 1), cov4);

    /* ---- Datos para el PDF: tiempo real vs ETS a 730 kHz ---- */
    {
        uint16_t arr; uint32_t cov; double ferr, erms;
        run_ets(730000.0, &arr, &cov, &ferr, &erms);
        double fs = (double)TIMCLK / (arr + 1);
        osc_measure(buf, OSC_N_CAPTURE, &m);
        double tns = m.period / fs * 1e9;          /* período en ns */
        char p[512];
        snprintf(p, sizeof p, "%s/datos_ets.dat", docdir);
        FILE *f = fopen(p, "w");
        fprintf(f, "t_ns v\n");
        for (unsigned j = 0; j < OSC_W_ETS; j++)
            fprintf(f, "%.2f %.4f\n", (j + 0.5) * tns / OSC_ETS_BINS, out[j] * 3.3 / 65536);
        fclose(f);
        snprintf(p, sizeof p, "%s/datos_ets_rt.dat", docdir);
        f = fopen(p, "w");
        fprintf(f, "t_ns v\n");
        for (unsigned k = 0; k < OSC_N_CAPTURE; k++) {
            double tk = (k - m.t0) / fs * 1e9;
            if (tk >= 0 && tk <= 2 * tns) fprintf(f, "%.2f %.4f\n", tk, buf[k] * 3.3 / 65536);
        }
        fclose(f);

        /* Trama binaria de ejemplo para probar el visor de PC. */
        osc_frame_hdr_t h;
        uint32_t dt_ps = (uint32_t)(tns * 1000.0 / OSC_ETS_BINS);
        osc_frame_build(&h, OSC_MODE_ETS, OSC_F_FREQ_VALID | OSC_F_ETS_OK | OSC_F_TRIGGERED,
                        135, out, OSC_W_ETS, 0, dt_ps, (uint32_t)fs,
                        (uint32_t)(fs / m.period * 1000.0), 32768, &m,
                        (uint16_t)(cov * 1000 / OSC_ETS_BINS));
        snprintf(p, sizeof p, "%s/trama_prueba.bin", bindir);
        f = fopen(p, "wb");
        fputs("OSC | ETS | texto de estado mezclado con tramas\r\n", f);
        fwrite(&h, sizeof h, 1, f);
        fwrite(out, sizeof out[0], OSC_W_ETS, f);
        fclose(f);
        CHECK(sizeof h == 38, "encabezado de trama de %zu bytes", sizeof h);
        printf("\nDatos del PDF -> %s/datos_ets*.dat, trama -> %s\n", docdir, p);
    }

    printf("\n%s (%d fallas)\n", s_fail ? "HAY FALLAS" : "TODO OK", s_fail);
    return s_fail ? 1 : 0;
}
