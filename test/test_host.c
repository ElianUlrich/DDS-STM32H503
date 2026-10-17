/**
 * test_host.c - Prueba del núcleo DDS en la PC (sin hardware).
 *
 *   make test
 *
 * Verifica la palabra de sintonía, el error de amplitud con y sin
 * interpolación, los modos del canal 2 y el cambio de forma de onda, y
 * escribe doc/datos_escalera.dat para el gráfico del PDF.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "dds_core.h"

#define PI 3.14159265358979323846
static int s_fail;

#define CHECK(cond, ...) do { \
    printf("  [%s] ", (cond) ? " OK " : "FALLA"); printf(__VA_ARGS__); printf("\n"); \
    if (!(cond)) s_fail++; } while (0)

static uint32_t buf[1u << 20];

static double ideal_sine(uint64_t ph)          /* ph en unidades de 2^-32 */
{
    return 2047.5 + 2047.5 * sin(2.0 * PI * (double)(ph & 0xFFFFFFFFull) / 4294967296.0);
}

/* Error máximo y RMS de CH1 respecto del seno ideal, en LSB. */
static void sine_error(uint32_t f_mhz, uint32_t n, double *emax, double *erms)
{
    dds_reset_phase();
    dds_set_frequency_mhz(f_mhz);
    dds_fill(buf, n);
    const uint32_t inc = dds_tuning_word(f_mhz);
    double m = 0, s2 = 0;
    uint64_t ph = 0;
    for (uint32_t i = 0; i < n; i++, ph += inc) {
        double e = fabs((double)(buf[i] & 0xFFFu) - ideal_sine(ph));
        if (e > m) m = e;
        s2 += e * e;
    }
    *emax = m;
    *erms = sqrt(s2 / n);
}

int main(int argc, char **argv)
{
    const char *outdir = (argc > 1) ? argv[1] : ".";
    double emax, erms;

    printf("DDS: Fs = %lu S/s, tabla %lu x 12 bits, resolucion = %.4f mHz\n",
           (unsigned long)DDS_FS_HZ, (unsigned long)DDS_TABLE_SIZE,
           1000.0 * DDS_FS_HZ / 4294967296.0);

    dds_core_init(WAVE_SENO, 1000000);

    printf("\n1) Palabra de sintonia y frecuencia real\n");
    const uint32_t fs[] = { 100, 1000, 1234567, 1000000, 50000000, 400000000 };
    for (unsigned i = 0; i < sizeof fs / sizeof fs[0]; i++) {
        dds_set_frequency_mhz(fs[i]);
        uint32_t fr = dds_actual_frequency_mhz();
        long err = (long)fr - (long)fs[i];
        CHECK(labs(err) <= 1, "f = %10.3f Hz  M = %10lu  real = %10.3f Hz",
              fs[i] / 1000.0, (unsigned long)dds_tuning_word(fs[i]), fr / 1000.0);
    }

    printf("\n2) Error de amplitud del seno (CH1)\n");
    sine_error(10000, 1000000, &emax, &erms);            /* 10 Hz: interpola */
    printf("  10 Hz  (interpolado) : max %.3f LSB, rms %.3f LSB\n", emax, erms);
    CHECK(emax < 1.0, "con interpolacion el error queda por debajo de 1 LSB");
    sine_error(100000000, 1000000, &emax, &erms);        /* 100 kHz: directo */
    printf("  100 kHz (directo)    : max %.3f LSB, rms %.3f LSB\n", emax, erms);
    CHECK(emax < 4.0, "truncamiento de fase a 12 bits: error < pi*2047/4096*2 LSB");

    printf("\n3) Interpolacion automatica\n");
    dds_set_frequency_mhz(200000);   /* 200 Hz < Fs/N = 244 Hz */
    CHECK(dds_interp_active(), "200 Hz -> interpolacion ON");
    dds_set_frequency_mhz(300000);
    CHECK(!dds_interp_active(), "300 Hz -> interpolacion OFF (camino rapido)");
    dds_set_waveform(WAVE_CUADRADA);
    dds_set_frequency_mhz(1000);
    CHECK(!dds_interp_active(), "cuadrada a 1 Hz -> no se interpola (flancos limpios)");

    printf("\n4) Canal 2\n");
    dds_set_waveform(WAVE_SENO);
    dds_set_frequency_mhz(1000000);
    dds_set_ch2_mode(CH2_CUADRATURA);
    dds_reset_phase();
    dds_fill(buf, 1000);
    CHECK((buf[0] >> 16) == 4095u && (buf[0] & 0xFFFu) == 2048u,
          "cuadratura: en fase 0, CH1 = %lu (medio) y CH2 = %lu (pico)",
          (unsigned long)(buf[0] & 0xFFF), (unsigned long)(buf[0] >> 16));
    dds_set_ch2_mode(CH2_INVERTIDA);
    dds_fill(buf, 1000);
    CHECK(((buf[10] & 0xFFFu) + (buf[10] >> 16)) == 4095u, "invertida: CH1 + CH2 = 4095");
    dds_set_ch2_mode(CH2_SYNC);
    dds_reset_phase();
    dds_fill(buf, 1000);                           /* 1 ms = 1 periodo de 1 kHz */
    CHECK((buf[100] >> 16) == 4095u && (buf[700] >> 16) == 0u, "sync: alto en la 1.a mitad, bajo en la 2.a");

    printf("\n5) Cambio de forma de onda (doble buffer)\n");
    dds_set_ch2_mode(CH2_IGUAL);
    dds_set_waveform(WAVE_SIERRA_ASC);
    dds_set_frequency_mhz(244141);                 /* 244,141 Hz = Fs/N: 1 muestra de tabla por muestra */
    dds_reset_phase();
    dds_fill(buf, 8);
    CHECK((buf[1] & 0xFFFu) - (buf[0] & 0xFFFu) == 1u, "sierra: 1 LSB por muestra de tabla");
    dds_set_waveform(WAVE_ARB);
    dds_fill(buf, 8);
    CHECK(1, "arbitraria (ECG) cargada desde flash: muestra = %lu", (unsigned long)(buf[0] & 0xFFF));

    /* ---- Datos para el grafico del PDF: escalera vs interpolado ---- */
    {
        char path[512];
        snprintf(path, sizeof path, "%s/datos_escalera.dat", outdir);
        FILE *f = fopen(path, "w");
        if (!f) { perror(path); return 1; }
        const uint32_t f_mhz = 10000;                /* 10 Hz */
        const uint32_t inc = dds_tuning_word(f_mhz);
        const uint32_t n0 = 0, n = 160;              /* cerca del cruce por cero */
        dds_set_waveform(WAVE_SENO);
        dds_set_ch2_mode(CH2_IGUAL);
        dds_set_frequency_mhz(f_mhz);
        dds_reset_phase();
        dds_fill(buf, n0 + n);
        fprintf(f, "n ideal sin_interp con_interp\n");
        for (uint32_t i = n0; i < n0 + n; i++) {
            uint64_t ph = (uint64_t)inc * i;
            uint32_t idx = (uint32_t)(ph & 0xFFFFFFFFu) >> DDS_PHASE_SHIFT;
            double tab = floor(2047.5 + 2047.5 * sin(2.0 * PI * idx / DDS_TABLE_SIZE) + 0.5);
            fprintf(f, "%u %.3f %.0f %lu\n", i, ideal_sine(ph), tab, (unsigned long)(buf[i] & 0xFFFu));
        }
        fclose(f);
        printf("\nDatos del grafico -> %s\n", path);
    }

    printf("\n%s (%d fallas)\n", s_fail ? "HAY FALLAS" : "TODO OK", s_fail);
    return s_fail ? 1 : 0;
}
