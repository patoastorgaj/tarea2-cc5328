"""Verifica un CSV guardado por la GUI: periodicidad, promedio del ESP32 y picos.

Uso:
    python tools/verificar_log.py gui_python/logs/sesion_XXXX.csv [--theta 28.0] [--hyst 0.3]

El recalculo del promedio necesita que la temperatura cruda se haya transmitido.
"""

import argparse
import csv
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "gui_python"))
from protocol import reference_peaks  # noqa: E402

AVG_TOL_C = 0.006  # la T llega con 2 decimales y el promedio con 3


def _float(row, key):
    val = row.get(key, "")
    return float(val) if val not in ("", None) else None


def load(path):
    samples, peaks = [], []
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if row["kind"] == "sample":
                samples.append(row)
            elif row["kind"] == "peak":
                peaks.append(row)
    return samples, peaks


def check_period(samples):
    print("== Periodicidad (diferencias de t_ms entre paquetes consecutivos) ==")
    dts = []
    for a, b in zip(samples, samples[1:]):
        if int(b["seq"]) == int(a["seq"]) + 1:
            dts.append(int(b["t_ms"]) - int(a["t_ms"]))
    if not dts:
        print("  sin datos suficientes")
        return
    hist = Counter(round(dt, -1) for dt in dts)
    for dt, n in sorted(hist.items()):
        print(f"  ~{dt:5d} ms : {n} paquetes")


def check_average(samples):
    print("== Promedio temporal: ESP32 vs recalculo en el PC ==")
    temps = [(int(r["t_ms"]), _float(r, "temperature_c")) for r in samples
             if _float(r, "temperature_c") is not None]
    if not temps:
        print("  no hay temperatura cruda en el registro, no se puede recalcular")
        return
    first_t = temps[0][0]
    checked, max_err = 0, 0.0
    for r in samples:
        avg = _float(r, "temp_avg_c")
        if avg is None:
            continue
        t, w = int(r["t_ms"]), int(r["avg_window_s"])
        # solo se compara si el PC tiene toda la ventana registrada
        if t - w * 1000 < first_t:
            continue
        vals = [v for (ti, v) in temps if t - w * 1000 < ti <= t]
        if len(vals) != int(r["avg_n"]):
            continue
        ref = sum(vals) / len(vals)
        max_err = max(max_err, abs(ref - avg))
        checked += 1
    if checked == 0:
        print("  no hubo ventanas completas comparables")
        return
    estado = "OK" if max_err <= AVG_TOL_C else "REVISAR"
    print(f"  {checked} promedios comparados, error maximo {max_err:.4f} C -> {estado}")


def check_peaks(samples, peaks, theta, hyst):
    print("== Eventos de pico ==")
    for p in peaks:
        print(f"  t={int(p['t_ms']) / 1000:.3f} s  pico={p['peak_c']} C  umbral={p['threshold_c']} C")
    if theta is None:
        return
    series = [(int(r["t_ms"]), _float(r, "temperature_c")) for r in samples
              if _float(r, "temperature_c") is not None]
    ref = reference_peaks(series, theta, hyst)
    esp = {int(p["t_ms"]) for p in peaks}
    print(f"  recalculo PC con theta={theta}, h={hyst}: {len(ref)} picos")
    for t, v in ref:
        marca = "coincide" if t in esp else "NO reportado por el ESP32"
        print(f"    t={t / 1000:.3f} s  {v:.2f} C  ({marca})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--theta", type=float, default=None)
    ap.add_argument("--hyst", type=float, default=0.3)
    args = ap.parse_args()
    samples, peaks = load(args.csv)
    print(f"{len(samples)} paquetes sample, {len(peaks)} eventos de pico\n")
    check_period(samples)
    check_average(samples)
    check_peaks(samples, peaks, args.theta, args.hyst)


if __name__ == "__main__":
    main()
