#!/usr/bin/env python3
"""
gen_tabla.py - Graba una forma de onda arbitraria como tabla C para el DDS.

Genera inc/wave_arb.h con 4096 muestras de 12 bits (uint16_t) que el
firmware copia a RAM cuando se elige la forma "ARBITRARIA".

Uso:
    python3 tools/gen_tabla.py                      # ECG sintético (por defecto)
    python3 tools/gen_tabla.py --csv senal.csv      # señal medida/capturada
    python3 tools/gen_tabla.py --csv senal.csv --col 1 --out inc/wave_arb.h

El CSV debe contener UN período de la señal (cualquier cantidad de puntos,
cualquier escala). Se re-muestrea por interpolación lineal a 4096 puntos y
se normaliza a 0..4095. Solo usa la biblioteca estándar de Python.
"""
import argparse
import csv
import math
import os

N = 4096          # muestras por período (2^12, igual que DDS_TABLE_SIZE)
CODE_MAX = 4095   # DAC de 12 bits


def ecg_sintetico(n):
    """Latido cardíaco simplificado: suma de gaussianas (P, Q, R, S, T)."""
    ondas = [  # (amplitud, centro [fracción del período], ancho)
        (0.12, 0.20, 0.025),   # P
        (-0.10, 0.355, 0.008),  # Q
        (1.00, 0.375, 0.010),   # R
        (-0.25, 0.395, 0.009),  # S
        (0.30, 0.62, 0.045),   # T
    ]
    y = []
    for i in range(n):
        x = i / n
        v = sum(a * math.exp(-((x - c) / w) ** 2 / 2) for a, c, w in ondas)
        y.append(v)
    return y


def leer_csv(ruta, col):
    vals = []
    with open(ruta, newline="") as f:
        for fila in csv.reader(f):
            try:
                vals.append(float(fila[col]))
            except (ValueError, IndexError):
                continue  # salta encabezados o filas vacías
    if len(vals) < 2:
        raise SystemExit("El CSV no tiene suficientes datos numéricos")
    return vals


def remuestrear(vals, n):
    """Interpolación lineal periódica de len(vals) puntos a n puntos."""
    m = len(vals)
    out = []
    for i in range(n):
        pos = i * m / n
        k = int(pos)
        fr = pos - k
        out.append(vals[k] * (1 - fr) + vals[(k + 1) % m] * fr)
    return out


def normalizar(vals):
    lo, hi = min(vals), max(vals)
    if hi == lo:
        return [CODE_MAX // 2] * len(vals)
    return [round((v - lo) * CODE_MAX / (hi - lo)) for v in vals]


def escribir_header(codigos, ruta, origen):
    with open(ruta, "w") as f:
        f.write("/* Generado por tools/gen_tabla.py - NO editar a mano.\n")
        f.write(f" * Origen: {origen}\n")
        f.write(f" * {len(codigos)} muestras de 12 bits ({2 * len(codigos)} bytes en flash). */\n")
        f.write("#ifndef WAVE_ARB_H\n#define WAVE_ARB_H\n\n#include <stdint.h>\n\n")
        f.write(f"static const uint16_t k_wave_arb[{len(codigos)}] = {{\n")
        for i in range(0, len(codigos), 16):
            f.write("    " + ", ".join(f"{c:4d}" for c in codigos[i:i + 16]) + ",\n")
        f.write("};\n\n#endif /* WAVE_ARB_H */\n")


def main():
    raiz = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv", help="archivo CSV con un período de la señal")
    ap.add_argument("--col", type=int, default=0, help="columna del CSV (desde 0)")
    ap.add_argument("--out", default=os.path.join(raiz, "inc", "wave_arb.h"))
    a = ap.parse_args()

    if a.csv:
        vals, origen = remuestrear(leer_csv(a.csv, a.col), N), os.path.basename(a.csv)
    else:
        vals, origen = ecg_sintetico(N), "ECG sintetico (P-QRS-T)"
    escribir_header(normalizar(vals), a.out, origen)
    print(f"OK: {a.out} ({N} muestras)")


if __name__ == "__main__":
    main()
