#!/usr/bin/env python3
"""
osciloscopio.py - Visor en la PC del osciloscopio del STM32H503.

Lee por el puerto serie (2 Mbaud) las tramas que manda el micro y las
dibuja. Las líneas de texto de estado se muestran en la consola.

Uso:
    pip install pyserial matplotlib
    python3 tools/osciloscopio.py --puerto COM5            # Windows
    python3 tools/osciloscopio.py --puerto /dev/ttyACM0    # Linux
    python3 tools/osciloscopio.py --puerto COM5 --csv captura.csv   # guarda la última trama

Sin placa (para probar el visor):
    python3 tools/osciloscopio.py --archivo build/trama_prueba.bin --texto

En el micro: mantener apretado el encoder FORMA para pasar a la página
del osciloscopio (empieza a adquirir solo).
"""
import argparse
import struct
import sys
from array import array

# Encabezado de trama: igual a osc_frame_hdr_t en inc/osc_core.h (38 bytes).
HDR = struct.Struct("<4sBBBBHHIIIHHHHHHH")
MAGIC = b"\xaa\x55OS"
CAMPOS = ("magic", "version", "modo", "flags", "bits_x10", "n", "trig_index",
          "dt_ps", "fs_hz", "freq_mhz", "trig_level", "vmin", "vmax", "vavg",
          "vrms", "cobertura", "checksum")
MODOS = {0: "AUTO", 1: "NORMAL", 2: "ETS"}
F_TRIG, F_FREQ, F_ETS_OK = 0x01, 0x02, 0x04
VREF = 3.3


def volts(code):
    return code * VREF / 65536.0


class Parser:
    """Separa el flujo serie en líneas de texto y tramas binarias."""

    def __init__(self):
        self.buf = bytearray()
        self.texto = bytearray()

    def _emit_text(self, data, out):
        self.texto += data
        while b"\n" in self.texto:
            linea, _, resto = self.texto.partition(b"\n")
            s = linea.decode("utf-8", errors="replace").strip()
            if s:
                out.append(("texto", s))
            self.texto = bytearray(resto)

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                keep = len(MAGIC) - 1          # puede haber medio "magic" al final
                if len(self.buf) > keep:
                    self._emit_text(self.buf[:-keep], out)
                    del self.buf[:-keep]
                break
            if i > 0:
                self._emit_text(self.buf[:i], out)
                del self.buf[:i]
            if len(self.buf) < HDR.size:
                break
            h = dict(zip(CAMPOS, HDR.unpack_from(self.buf)))
            if h["version"] != 1 or h["n"] == 0 or h["n"] > 4096:
                del self.buf[:1]               # coincidencia falsa
                continue
            total = HDR.size + 2 * h["n"]
            if len(self.buf) < total:
                break
            muestras = array("H")
            muestras.frombytes(bytes(self.buf[HDR.size:total]))
            if sys.byteorder != "little":
                muestras.byteswap()
            if (sum(muestras) & 0xFFFF) != h["checksum"]:
                del self.buf[:1]               # trama corrupta
                continue
            del self.buf[:total]
            out.append(("trama", h, muestras))
        return out


def unidad_tiempo(t_total_s):
    if t_total_s < 2e-6:
        return 1e9, "ns"
    if t_total_s < 2e-3:
        return 1e6, "µs"
    if t_total_s < 2:
        return 1e3, "ms"
    return 1.0, "s"


def formato_freq(mhz):
    hz = mhz / 1000.0
    if hz >= 1e6:
        return f"{hz / 1e6:.4f} MHz"
    if hz >= 1e3:
        return f"{hz / 1e3:.4f} kHz"
    return f"{hz:.3f} Hz"


def resumen(h):
    partes = [MODOS.get(h["modo"], "?"),
              f"Fs {h['fs_hz'] / 1e6:.3f} MS/s",
              f"{h['bits_x10'] / 10:.1f} bits",
              f"dt {h['dt_ps'] / 1000:.2f} ns"]
    if h["flags"] & F_FREQ:
        partes.append("f " + formato_freq(h["freq_mhz"]))
    partes.append(f"Vpp {volts(h['vmax'] - h['vmin']):.3f} V")
    partes.append(f"Vmed {volts(h['vavg']):.3f} V")
    partes.append(f"Vrms {volts(h['vrms']):.3f} V")
    if h["modo"] == 2:
        partes.append(f"cobertura {h['cobertura'] / 10:.1f} %")
    partes.append("TRIG" if h["flags"] & F_TRIG else "sin trigger")
    return " | ".join(partes)


def guardar_csv(ruta, h, muestras):
    with open(ruta, "w") as f:
        f.write("t_s,v\n")
        for i, c in enumerate(muestras):
            f.write(f"{i * h['dt_ps'] * 1e-12:.12e},{volts(c):.5f}\n")


def fuente_archivo(ruta):
    with open(ruta, "rb") as f:
        yield f.read()


def fuente_serie(puerto, baud):
    try:
        import serial  # pyserial
    except ImportError:
        sys.exit("Falta pyserial:  pip install pyserial")
    with serial.Serial(puerto, baud, timeout=0.05) as s:
        while True:
            yield s.read(max(1, s.in_waiting))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--puerto", help="puerto serie (COM5, /dev/ttyACM0, ...)")
    ap.add_argument("--baud", type=int, default=2000000)
    ap.add_argument("--archivo", help="leer un volcado binario en vez del puerto")
    ap.add_argument("--texto", action="store_true", help="no graficar, solo imprimir")
    ap.add_argument("--csv", help="guardar la última trama en este CSV")
    a = ap.parse_args()
    if not a.puerto and not a.archivo:
        ap.error("indicá --puerto o --archivo")

    fuente = fuente_archivo(a.archivo) if a.archivo else fuente_serie(a.puerto, a.baud)
    parser = Parser()

    plt = linea = ax = nivel = None
    if not a.texto:
        try:
            import matplotlib.pyplot as plt
        except ImportError:
            sys.exit("Falta matplotlib:  pip install matplotlib  (o usá --texto)")
        plt.ion()
        fig, ax = plt.subplots(figsize=(10, 5))
        fig.canvas.manager.set_window_title("Osciloscopio STM32H503")
        (linea,) = ax.plot([], [], lw=1.2)
        nivel = ax.axhline(VREF / 2, ls="--", lw=0.8, color="tab:red")
        ax.set_ylim(-0.05, VREF + 0.05)
        ax.set_ylabel("V")
        ax.grid(True, alpha=0.4)

    tramas = 0
    for datos in fuente:
        for evento in parser.feed(datos):
            if evento[0] == "texto":
                print("[micro]", evento[1])
                continue
            _, h, muestras = evento
            tramas += 1
            if a.csv:
                guardar_csv(a.csv, h, muestras)
            if a.texto:
                print(f"trama {tramas}: {h['n']} puntos | {resumen(h)}")
                continue
            esc, uni = unidad_tiempo(h["n"] * h["dt_ps"] * 1e-12)
            t = [i * h["dt_ps"] * 1e-12 * esc for i in range(h["n"])]
            linea.set_data(t, [volts(c) for c in muestras])
            nivel.set_ydata([volts(h["trig_level"])] * 2)
            ax.set_xlim(0, t[-1] if t else 1)
            ax.set_xlabel(f"tiempo [{uni}]")
            ax.set_title(resumen(h), fontsize=9)
        if plt is not None:
            plt.pause(0.001)
            if not plt.get_fignums():
                break
    if plt is not None and a.archivo:
        plt.ioff()
        plt.show()


if __name__ == "__main__":
    main()
