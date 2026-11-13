"""Lectura y escritura serial en un hilo aparte para no bloquear la GUI."""

import queue

import serial
from serial.tools import list_ports
from PyQt6.QtCore import QThread, pyqtSignal


def available_ports():
    return [(p.device, p.description) for p in list_ports.comports()]


class SerialWorker(QThread):
    line_received = pyqtSignal(str)
    line_sent = pyqtSignal(str)
    opened = pyqtSignal(str)
    closed = pyqtSignal(str)  # motivo del cierre

    def __init__(self, port, baudrate, parent=None):
        super().__init__(parent)
        self.port = port
        self.baudrate = baudrate
        self._tx = queue.Queue()
        self._running = True

    def send(self, text):
        self._tx.put(text)

    def stop(self):
        self._running = False

    def run(self):
        ser = serial.Serial()
        ser.port = self.port
        ser.baudrate = self.baudrate
        ser.timeout = 0.05
        # DTR/RTS en falso para no dejar el ESP32 en reset al abrir
        ser.dtr = False
        ser.rts = False
        try:
            ser.open()
        except (serial.SerialException, OSError) as e:
            self.closed.emit(f"No se pudo abrir {self.port}: {e}")
            return

        self.opened.emit(self.port)
        reason = "Desconectado por el usuario"
        buf = b""
        try:
            while self._running:
                while not self._tx.empty():
                    text = self._tx.get_nowait()
                    ser.write((text + "\n").encode("utf-8"))
                    self.line_sent.emit(text)
                data = ser.read(ser.in_waiting or 1)
                if not data:
                    continue
                buf += data
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", errors="replace").strip()
                    if line:
                        self.line_received.emit(line)
                # evita crecer sin limite si llega basura sin saltos de linea
                if len(buf) > 4096:
                    buf = b""
        except (serial.SerialException, OSError) as e:
            reason = f"Se perdió la conexión: {e}"
        finally:
            try:
                ser.close()
            except Exception:
                pass
        self.closed.emit(reason)
