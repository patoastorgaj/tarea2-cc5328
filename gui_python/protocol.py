"""Mensajes del protocolo JSON Lines y calculos de validacion en el PC."""

import json
from collections import deque

SAMPLING_HZ = (0.5, 1, 2, 5)
WINDOWS_S = (5, 10, 30, 60)
VARIABLES = ("T", "H", "P")

# campos de cada variable dentro de un paquete sample
FIELDS = {
    "T": ("temperature_c", "Temperatura", "°C"),
    "H": ("humidity_pct", "Humedad", "%HR"),
    "P": ("pressure_hpa", "Presión", "hPa"),
}

MESSAGE_TYPES = {"hello", "status", "ack", "error", "sample", "peak", "pong", "selftest"}


def parse_line(line):
    """Retorna el mensaje como dict, o None si la linea no es del protocolo."""
    line = line.strip()
    if not line.startswith("{"):
        return None
    try:
        msg = json.loads(line)
    except ValueError:
        return None
    if not isinstance(msg, dict) or msg.get("type") not in MESSAGE_TYPES:
        return None
    return msg


def _dump(obj):
    return json.dumps(obj, separators=(",", ":"))


def config_command(sampling_hz, variables, avg_enabled, window_s,
                   peak_enabled, threshold_c, hysteresis_c, cmd_id=None):
    """Arma el comando config completo. Lanza ValueError si la seleccion es invalida."""
    variables = [v for v in VARIABLES if v in variables]
    if not variables:
        raise ValueError("Debe seleccionar al menos una variable")
    if sampling_hz not in SAMPLING_HZ:
        raise ValueError(f"Frecuencia no permitida: {sampling_hz}")
    cmd = {
        "type": "config",
        "sampling_hz": sampling_hz,
        "variables": variables,
        "average": {"enabled": bool(avg_enabled), "window_s": int(window_s)},
        "peak": {
            "enabled": bool(peak_enabled),
            "threshold_c": round(float(threshold_c), 1),
            "hysteresis_c": round(float(hysteresis_c), 1),
        },
    }
    if cmd_id:
        cmd["id"] = cmd_id
    return _dump(cmd)


def simple_command(kind, cmd_id=None):
    cmd = {"type": kind}
    if cmd_id:
        cmd["id"] = cmd_id
    return _dump(cmd)


class ReferenceAverage:
    """Media temporal recalculada en el PC, solo para validar la del ESP32."""

    def __init__(self, max_window_s=120):
        self.max_ms = max_window_s * 1000
        self.points = deque()

    def clear(self):
        self.points.clear()

    def add(self, t_ms, temp_c):
        self.points.append((t_ms, temp_c))
        while self.points and self.points[0][0] <= t_ms - self.max_ms:
            self.points.popleft()

    def mean(self, now_ms, window_s):
        start = now_ms - window_s * 1000
        vals = [v for t, v in self.points if start < t <= now_ms]
        if not vals:
            return None, 0
        return sum(vals) / len(vals), len(vals)


def reference_peaks(samples, threshold_c, hysteresis_c=0.0):
    """Mismo criterio del firmware sobre una lista de (t_ms, temp). Retorna [(t_ms, temp)]."""
    peaks = []
    window = []
    armed = True
    for t, v in samples:
        window.append((t, v))
        if len(window) > 3:
            window.pop(0)
        if len(window) == 3 and armed:
            a, b, c = window[0][1], window[1][1], window[2][1]
            if b > a and b >= c and b >= threshold_c:
                peaks.append(window[1])
                armed = hysteresis_c <= 0
        if not armed and v < threshold_c - hysteresis_c:
            armed = True
    return peaks


class RateMeter:
    """Estima la frecuencia real a partir de las marcas t_ms del ESP32."""

    def __init__(self, n=10):
        self.times = deque(maxlen=n)

    def clear(self):
        self.times.clear()

    def add(self, t_ms):
        self.times.append(t_ms)

    def hz(self):
        if len(self.times) < 2:
            return None
        span = self.times[-1] - self.times[0]
        if span <= 0:
            return None
        return (len(self.times) - 1) * 1000.0 / span

    def period_ms(self):
        hz = self.hz()
        return None if hz is None else 1000.0 / hz
