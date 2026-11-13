"""Ventana principal de la GUI."""

import csv
import time
from collections import deque
from datetime import datetime
from pathlib import Path

import pyqtgraph as pg
from PyQt6.QtCore import Qt, QTimer
from PyQt6.QtGui import QFont
from PyQt6.QtWidgets import (
    QCheckBox, QComboBox, QDoubleSpinBox, QFormLayout, QGridLayout, QGroupBox,
    QHBoxLayout, QHeaderView, QLabel, QLineEdit, QMainWindow, QPlainTextEdit,
    QPushButton, QScrollArea, QSpinBox, QTableWidget, QTableWidgetItem,
    QTabWidget, QVBoxLayout, QWidget,
)

import protocol as proto
from serial_worker import SerialWorker, available_ports

BAUDRATES = (9600, 57600, 115200, 230400, 460800, 921600)
HISTORY_LEN = 1500
ACK_TIMEOUT_MS = 2000
RECONNECT_MS = 2000
LOG_DIR = Path(__file__).resolve().parent / "logs"

CSV_FIELDS = ["kind", "pc_time", "seq", "t_ms", "valid", "error", "temperature_c",
              "humidity_pct", "pressure_hpa", "temp_avg_c", "avg_n", "avg_window_s",
              "avg_window_complete", "peak_c", "threshold_c"]

COLOR_OK = "#1b7f3b"
COLOR_WARN = "#b36b00"
COLOR_ERR = "#b00020"
COLOR_OFF = "#888888"


def _big_label(text="—"):
    lbl = QLabel(text)
    f = QFont()
    f.setPointSize(15)
    f.setBold(True)
    lbl.setFont(f)
    return lbl


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Tarea 2 - BME688 / ESP32")
        self.resize(1400, 880)

        self.worker = None
        self.user_disconnect = False
        self.last_port = None
        self.cmd_counter = 0
        self.pending = {}           # id -> descripcion del comando
        self.confirmed = None       # ultima config confirmada por el ESP32
        self.last_sample_pc = None  # time.monotonic() del ultimo paquete
        self.last_seq = None
        self.lost_packets = 0
        self.invalid_count = 0
        self.peak_count = 0
        self.csv_file = None
        self.csv_writer = None

        self.ref_avg = proto.ReferenceAverage()
        self.rate = proto.RateMeter()
        self.data = {v: deque(maxlen=HISTORY_LEN) for v in proto.VARIABLES}
        self.avg_data = deque(maxlen=HISTORY_LEN)
        self.peak_points = deque(maxlen=200)

        self._build_ui()

        self.watchdog = QTimer(self)
        self.watchdog.timeout.connect(self._check_data_timeout)
        self.watchdog.start(500)

        self.reconnect_timer = QTimer(self)
        self.reconnect_timer.setInterval(RECONNECT_MS)
        self.reconnect_timer.timeout.connect(self._try_reconnect)

        self.refresh_ports()
        self._update_connection_state(False, "Desconectado")

    # construccion de la interfaz

    def _build_ui(self):
        root = QWidget()
        layout = QHBoxLayout(root)

        controls = QWidget()
        cl = QVBoxLayout(controls)
        cl.addWidget(self._build_connection_box())
        cl.addWidget(self._build_acquisition_box())
        cl.addWidget(self._build_processing_box())
        cl.addWidget(self._build_confirmed_box())
        cl.addWidget(self._build_tools_box())
        cl.addStretch()
        scroll = QScrollArea()
        scroll.setWidget(controls)
        scroll.setWidgetResizable(True)
        scroll.setFixedWidth(370)
        layout.addWidget(scroll)

        right = QVBoxLayout()
        right.addWidget(self._build_readouts())
        right.addWidget(self._build_plots(), stretch=3)
        right.addWidget(self._build_tabs(), stretch=1)
        layout.addLayout(right, stretch=1)

        self.setCentralWidget(root)
        self.statusBar().showMessage("Listo")

    def _build_connection_box(self):
        box = QGroupBox("Conexión")
        f = QFormLayout(box)
        row = QHBoxLayout()
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(170)
        btn_refresh = QPushButton("Actualizar")
        btn_refresh.clicked.connect(self.refresh_ports)
        row.addWidget(self.port_combo)
        row.addWidget(btn_refresh)
        f.addRow("Puerto:", row)

        self.baud_combo = QComboBox()
        for b in BAUDRATES:
            self.baud_combo.addItem(str(b), b)
        self.baud_combo.setCurrentText("115200")
        f.addRow("Baudrate:", self.baud_combo)

        self.btn_connect = QPushButton("Conectar")
        self.btn_connect.clicked.connect(self.toggle_connection)
        f.addRow(self.btn_connect)

        self.chk_reconnect = QCheckBox("Reconectar automáticamente")
        self.chk_reconnect.setChecked(True)
        f.addRow(self.chk_reconnect)
        self.chk_resend = QCheckBox("Reenviar configuración si el ESP32 reinicia")
        self.chk_resend.setChecked(True)
        f.addRow(self.chk_resend)

        self.lbl_conn = QLabel()
        self.lbl_device = QLabel("Dispositivo: —")
        self.lbl_sensor = QLabel("Sensor: —")
        self.lbl_data = QLabel("Datos: —")
        for w in (self.lbl_conn, self.lbl_device, self.lbl_sensor, self.lbl_data):
            w.setWordWrap(True)
            f.addRow(w)
        return box

    def _build_acquisition_box(self):
        box = QGroupBox("Adquisición")
        f = QFormLayout(box)
        self.hz_combo = QComboBox()
        for hz in proto.SAMPLING_HZ:
            self.hz_combo.addItem(f"{hz} Hz", hz)
        self.hz_combo.setCurrentIndex(1)
        f.addRow("Frecuencia:", self.hz_combo)

        row = QHBoxLayout()
        self.var_checks = {}
        for v in proto.VARIABLES:
            chk = QCheckBox(proto.FIELDS[v][1])
            chk.setChecked(True)
            chk.toggled.connect(self._validate_selection)
            self.var_checks[v] = chk
            row.addWidget(chk)
        f.addRow("Transmitir:", row)
        self.lbl_sel_warn = QLabel("")
        self.lbl_sel_warn.setStyleSheet(f"color:{COLOR_ERR}")
        f.addRow(self.lbl_sel_warn)
        return box

    def _build_processing_box(self):
        box = QGroupBox("Procesamiento en el ESP32")
        f = QFormLayout(box)
        self.chk_avg = QCheckBox("Promedio temporal de T")
        self.chk_avg.setChecked(True)
        f.addRow(self.chk_avg)
        self.window_spin = QSpinBox()
        self.window_spin.setRange(1, 120)
        self.window_spin.setValue(10)
        self.window_spin.setSuffix(" s")
        row = QHBoxLayout()
        row.addWidget(self.window_spin)
        for w in proto.WINDOWS_S:
            b = QPushButton(str(w))
            b.setFixedWidth(34)
            b.clicked.connect(lambda _=False, w=w: self.window_spin.setValue(w))
            row.addWidget(b)
        f.addRow("Ventana W:", row)

        self.chk_peak = QCheckBox("Detección de picos de T")
        self.chk_peak.setChecked(True)
        f.addRow(self.chk_peak)
        self.threshold_spin = QDoubleSpinBox()
        self.threshold_spin.setRange(-40.0, 85.0)
        self.threshold_spin.setDecimals(1)
        self.threshold_spin.setSingleStep(0.1)
        self.threshold_spin.setValue(28.0)
        self.threshold_spin.setSuffix(" °C")
        f.addRow("Umbral θ:", self.threshold_spin)
        self.hyst_spin = QDoubleSpinBox()
        self.hyst_spin.setRange(0.0, 5.0)
        self.hyst_spin.setDecimals(1)
        self.hyst_spin.setSingleStep(0.1)
        self.hyst_spin.setValue(0.3)
        self.hyst_spin.setSuffix(" °C")
        f.addRow("Histéresis:", self.hyst_spin)

        self.btn_apply = QPushButton("Enviar configuración al ESP32")
        self.btn_apply.clicked.connect(self.send_config)
        f.addRow(self.btn_apply)
        self.lbl_pending = QLabel("")
        self.lbl_pending.setWordWrap(True)
        f.addRow(self.lbl_pending)
        return box

    def _build_confirmed_box(self):
        box = QGroupBox("Configuración confirmada por el ESP32")
        v = QVBoxLayout(box)
        self.lbl_confirmed = QLabel("Sin confirmación todavía")
        self.lbl_confirmed.setWordWrap(True)
        self.lbl_confirmed.setTextFormat(Qt.TextFormat.RichText)
        v.addWidget(self.lbl_confirmed)
        return box

    def _build_tools_box(self):
        box = QGroupBox("Pruebas y comandos")
        g = QGridLayout(box)
        self.btn_stream = QPushButton("Detener transmisión")
        self.btn_stream.clicked.connect(self.toggle_streaming)
        g.addWidget(self.btn_stream, 0, 0)
        btn_get = QPushButton("Leer config")
        btn_get.clicked.connect(lambda: self.send_simple("get_config"))
        g.addWidget(btn_get, 0, 1)
        btn_self = QPushButton("Autotest ESP32")
        btn_self.clicked.connect(lambda: self.send_simple("selftest"))
        g.addWidget(btn_self, 1, 0)
        btn_bad = QPushButton("Enviar config inválida")
        btn_bad.clicked.connect(self.send_invalid_config)
        g.addWidget(btn_bad, 1, 1)
        self.raw_edit = QLineEdit()
        self.raw_edit.setPlaceholderText('{"type":"ping"}')
        self.raw_edit.returnPressed.connect(self.send_raw)
        g.addWidget(self.raw_edit, 2, 0)
        btn_raw = QPushButton("Enviar texto")
        btn_raw.clicked.connect(self.send_raw)
        g.addWidget(btn_raw, 2, 1)
        self.chk_csv = QCheckBox("Guardar sesión en CSV")
        self.chk_csv.toggled.connect(self.toggle_csv)
        g.addWidget(self.chk_csv, 3, 0, 1, 2)
        btn_clear = QPushButton("Limpiar gráficos")
        btn_clear.clicked.connect(self.clear_data)
        g.addWidget(btn_clear, 4, 0, 1, 2)
        return box

    def _build_readouts(self):
        box = QGroupBox("Valores actuales")
        g = QGridLayout(box)
        self.val_labels = {}
        for col, v in enumerate(proto.VARIABLES):
            name = proto.FIELDS[v][1]
            g.addWidget(QLabel(f"{name} [{proto.FIELDS[v][2]}]"), 0, col)
            lbl = _big_label()
            g.addWidget(lbl, 1, col)
            self.val_labels[v] = lbl

        g.addWidget(QLabel("Promedio T (ESP32) [°C]"), 0, 3)
        self.lbl_avg = _big_label()
        g.addWidget(self.lbl_avg, 1, 3)
        g.addWidget(QLabel("Validación PC [°C]"), 0, 4)
        self.lbl_ref = _big_label()
        self.lbl_ref.setStyleSheet(f"color:{COLOR_OFF}")
        g.addWidget(self.lbl_ref, 1, 4)

        self.lbl_avg_info = QLabel("Ventana: —")
        self.lbl_rate = QLabel("Frecuencia medida: —")
        self.lbl_seq = QLabel("seq: —   t_ms: —")
        self.lbl_threshold = QLabel("Umbral actual: —")
        self.lbl_counts = QLabel("Picos: 0   Inválidas: 0   Perdidos: 0")
        for i, w in enumerate((self.lbl_avg_info, self.lbl_rate, self.lbl_seq,
                               self.lbl_threshold, self.lbl_counts)):
            g.addWidget(w, 2 + i // 3, i % 3 if i < 3 else (i - 3) * 2, 1, 1 if i < 3 else 2)
        return box

    def _build_plots(self):
        pg.setConfigOptions(antialias=True, background="w", foreground="k")
        w = pg.GraphicsLayoutWidget()
        self.plots = {}
        self.curves = {}
        for row, v in enumerate(proto.VARIABLES):
            name, unit = proto.FIELDS[v][1], proto.FIELDS[v][2]
            p = w.addPlot(row=row, col=0)
            p.showGrid(x=True, y=True, alpha=0.3)
            p.setLabel("left", f"{name} [{unit}]")
            p.addLegend(offset=(10, 5))
            if row > 0:
                p.setXLink(self.plots["T"])
            self.plots[v] = p
            self.curves[v] = p.plot([], [], pen=pg.mkPen("#1f5fa8", width=2), name=name)
        self.plots["P"].setLabel("bottom", "Tiempo ESP32 [s]")

        pt = self.plots["T"]
        self.avg_curve = pt.plot([], [], pen=pg.mkPen("#d95f02", width=2, style=Qt.PenStyle.DashLine),
                                 name="Promedio (ESP32)")
        self.threshold_line = pg.InfiniteLine(angle=0, pen=pg.mkPen("#b00020", width=1,
                                                                   style=Qt.PenStyle.DotLine))
        pt.addItem(self.threshold_line)
        self.peak_scatter = pg.ScatterPlotItem(size=12, brush=pg.mkBrush("#b00020"), symbol="t1",
                                               name="Picos")
        pt.addItem(self.peak_scatter)
        return w

    def _build_tabs(self):
        tabs = QTabWidget()
        self.peak_table = QTableWidget(0, 5)
        self.peak_table.setHorizontalHeaderLabels(
            ["#", "Hora PC", "t pico ESP32 [s]", "Pico [°C]", "Umbral [°C]"])
        self.peak_table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.peak_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.peak_table.verticalHeader().setVisible(False)
        tabs.addTab(self.peak_table, "Eventos de pico")
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(3000)
        self.log.setFont(QFont("Monospace", 9))
        tabs.addTab(self.log, "Registro del protocolo")
        self.chk_log_samples = QCheckBox("Mostrar paquetes sample en el registro")
        tabs.setCornerWidget(self.chk_log_samples)
        return tabs

    # utilidades

    def log_msg(self, text, color=None):
        stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self.log.appendPlainText(f"[{stamp}] {text}")
        if color:
            self.statusBar().showMessage(text, 6000)

    def next_id(self, prefix="c"):
        self.cmd_counter += 1
        return f"{prefix}{self.cmd_counter}"

    def is_connected(self):
        return self.worker is not None and self.worker.isRunning()

    def selected_vars(self):
        return [v for v in proto.VARIABLES if self.var_checks[v].isChecked()]

    def _validate_selection(self):
        ok = bool(self.selected_vars())
        self.lbl_sel_warn.setText("" if ok else "Seleccione al menos una variable")
        self.btn_apply.setEnabled(ok)

    # conexion

    def refresh_ports(self):
        current = self.port_combo.currentData()
        self.port_combo.clear()
        for dev, desc in available_ports():
            self.port_combo.addItem(f"{dev} ({desc})" if desc and desc != "n/a" else dev, dev)
        if current:
            idx = self.port_combo.findData(current)
            if idx >= 0:
                self.port_combo.setCurrentIndex(idx)

    def toggle_connection(self):
        if self.is_connected():
            self.user_disconnect = True
            self.reconnect_timer.stop()
            self.worker.stop()
        else:
            port = self.port_combo.currentData()
            if not port:
                self.log_msg("No hay puerto seleccionado", COLOR_ERR)
                return
            self.open_port(port)

    def open_port(self, port):
        self.user_disconnect = False
        self.last_port = port
        self.worker = SerialWorker(port, self.baud_combo.currentData())
        self.worker.line_received.connect(self.on_line)
        self.worker.line_sent.connect(lambda t: self.log_msg(f">> {t}"))
        self.worker.opened.connect(self.on_opened)
        self.worker.closed.connect(self.on_closed)
        self.worker.start()
        self._update_connection_state(False, f"Conectando a {port}…", COLOR_WARN)

    def on_opened(self, port):
        self.reconnect_timer.stop()
        self.log_msg(f"Puerto {port} abierto", COLOR_OK)
        self._update_connection_state(True, f"Conectado a {port}", COLOR_OK)
        self.last_sample_pc = time.monotonic()
        # se pide la config actual para saber en que estado esta el ESP32
        QTimer.singleShot(300, lambda: self.send_simple("get_config"))

    def on_closed(self, reason):
        self.log_msg(reason, COLOR_ERR)
        self.worker = None
        self.pending.clear()
        self.lbl_pending.setText("")
        if self.user_disconnect or not self.chk_reconnect.isChecked():
            self._update_connection_state(False, f"Desconectado: {reason}", COLOR_OFF)
        else:
            self._update_connection_state(False, f"Conexión perdida, reintentando…", COLOR_ERR)
            self.reconnect_timer.start()

    def _try_reconnect(self):
        if self.is_connected() or not self.last_port:
            return
        self.refresh_ports()
        ports = [p for p, _ in available_ports()]
        if self.last_port in ports:
            self.log_msg(f"Reintentando conexión a {self.last_port}")
            self.open_port(self.last_port)

    def _update_connection_state(self, connected, text, color=COLOR_OFF):
        self.lbl_conn.setText(f"Estado: {text}")
        self.lbl_conn.setStyleSheet(f"color:{color}; font-weight:bold")
        self.btn_connect.setText("Desconectar" if connected or self.worker else "Conectar")
        self.port_combo.setEnabled(not connected)
        self.baud_combo.setEnabled(not connected)
        if not connected:
            self.lbl_data.setText("Datos: —")
            self.lbl_data.setStyleSheet("")

    # envio de comandos

    def send(self, text, cmd_id=None, desc=""):
        if not self.is_connected():
            self.log_msg("No conectado: comando no enviado", COLOR_ERR)
            return False
        self.worker.send(text)
        if cmd_id:
            self.pending[cmd_id] = desc
            self.lbl_pending.setText(f"Esperando confirmación de {desc} ({cmd_id})…")
            self.lbl_pending.setStyleSheet(f"color:{COLOR_WARN}")
            QTimer.singleShot(ACK_TIMEOUT_MS, lambda: self._check_ack_timeout(cmd_id))
        return True

    def _check_ack_timeout(self, cmd_id):
        if cmd_id in self.pending:
            desc = self.pending.pop(cmd_id)
            self.lbl_pending.setText(f"Sin respuesta del ESP32 para {desc} ({cmd_id})")
            self.lbl_pending.setStyleSheet(f"color:{COLOR_ERR}")
            self.log_msg(f"Timeout esperando respuesta a {cmd_id}", COLOR_ERR)

    def send_config(self):
        try:
            cmd_id = self.next_id("c")
            text = proto.config_command(
                self.hz_combo.currentData(), self.selected_vars(),
                self.chk_avg.isChecked(), self.window_spin.value(),
                self.chk_peak.isChecked(), self.threshold_spin.value(),
                self.hyst_spin.value(), cmd_id)
        except ValueError as e:
            self.log_msg(f"Configuración rechazada en la GUI: {e}", COLOR_ERR)
            return
        self.send(text, cmd_id, "config")

    def send_simple(self, kind):
        cmd_id = self.next_id(kind[0])
        self.send(proto.simple_command(kind, cmd_id), cmd_id, kind)

    def send_invalid_config(self):
        # frecuencia no permitida a proposito, el ESP32 debe rechazarla sin cambiar nada
        cmd_id = self.next_id("bad")
        self.send('{"type":"config","id":"%s","sampling_hz":3,"variables":["T"]}' % cmd_id,
                  cmd_id, "config inválida")

    def send_raw(self):
        text = self.raw_edit.text().strip()
        if text:
            self.send(text)

    def toggle_streaming(self):
        streaming = self.confirmed.get("streaming", True) if self.confirmed else True
        self.send_simple("stop" if streaming else "start")

    # recepcion

    def on_line(self, line):
        msg = proto.parse_line(line)
        if msg is None:
            self.log_msg(f"(ignorado) {line}")
            return
        kind = msg["type"]
        if kind != "sample" or self.chk_log_samples.isChecked():
            self.log_msg(f"<< {line}")
        handler = getattr(self, f"_on_{kind}", None)
        if handler:
            handler(msg)

    def _on_hello(self, msg):
        self.lbl_device.setText(f"Dispositivo: {msg.get('fw')} v{msg.get('version')}")
        self.log_msg("El ESP32 se inició (hello)", COLOR_WARN)
        self.last_seq = None
        self.rate.clear()
        self.ref_avg.clear()

    def _on_status(self, msg):
        if msg.get("fw"):
            self.lbl_device.setText(f"Dispositivo: {msg.get('fw')} v{msg.get('version')}")
        if msg.get("sensor") == "ok":
            self.lbl_sensor.setText(f"Sensor: {msg.get('variant')} en {msg.get('addr')}")
            self.lbl_sensor.setStyleSheet(f"color:{COLOR_OK}")
        else:
            self.lbl_sensor.setText(f"Sensor: error ({msg.get('detail')})")
            self.lbl_sensor.setStyleSheet(f"color:{COLOR_ERR}")

    def _on_ack(self, msg):
        cmd_id = msg.get("id")
        if cmd_id and cmd_id in self.pending:
            desc = self.pending.pop(cmd_id)
            self.lbl_pending.setText(f"{desc} confirmado ({cmd_id})")
            self.lbl_pending.setStyleSheet(f"color:{COLOR_OK}")
        self._apply_confirmed(msg)
        if msg.get("cmd") == "boot" and self.chk_resend.isChecked() and self.selected_vars():
            self.log_msg("Reinicio detectado, reenviando configuración de la GUI", COLOR_WARN)
            QTimer.singleShot(200, self.send_config)

    def _on_error(self, msg):
        cmd_id = msg.get("id")
        text = f"ESP32 rechazó el comando: {msg.get('code')}"
        if msg.get("field"):
            text += f" en '{msg['field']}'"
        text += f" ({msg.get('msg', '')})"
        if cmd_id and cmd_id in self.pending:
            self.pending.pop(cmd_id)
        self.lbl_pending.setText(text + ". La configuración no cambió.")
        self.lbl_pending.setStyleSheet(f"color:{COLOR_ERR}")
        self.log_msg(text, COLOR_ERR)

    def _on_pong(self, msg):
        self.pending.pop(msg.get("id"), None)
        self.lbl_pending.setText("pong recibido")

    def _on_selftest(self, msg):
        self.pending.pop(msg.get("id"), None)
        ok = msg.get("status") == "ok"
        text = (f"Autotest {'OK' if ok else 'FALLÓ'}: picos con histéresis={msg.get('peaks_hyst')} (esp. 3), "
                f"sin histéresis={msg.get('peaks_no_hyst')} (esp. 4), media W5={msg.get('avg_w5')} (esp. 27.0), "
                f"media W30={msg.get('avg_w30')} (esp. 24.5)")
        self.lbl_pending.setText(text)
        self.lbl_pending.setStyleSheet(f"color:{COLOR_OK if ok else COLOR_ERR}")
        self.log_msg(text, COLOR_OK if ok else COLOR_ERR)

    def _apply_confirmed(self, msg):
        self.confirmed = msg
        avg = msg.get("average", {})
        peak = msg.get("peak", {})
        variables = msg.get("variables", [])
        names = ", ".join(proto.FIELDS[v][1] for v in variables) or "ninguna"
        self.lbl_confirmed.setText(
            f"<b>Frecuencia:</b> {msg.get('sampling_hz')} Hz<br>"
            f"<b>Variables transmitidas:</b> {names}<br>"
            f"<b>Promedio:</b> {'activo' if avg.get('enabled') else 'inactivo'}, W = {avg.get('window_s')} s<br>"
            f"<b>Picos:</b> {'activo' if peak.get('enabled') else 'inactivo'}, "
            f"θ = {peak.get('threshold_c')} °C, histéresis = {peak.get('hysteresis_c')} °C<br>"
            f"<b>Transmisión:</b> {'en curso' if msg.get('streaming') else 'detenida'}")
        self.btn_stream.setText("Detener transmisión" if msg.get("streaming") else "Reanudar transmisión")
        self.lbl_threshold.setText(f"Umbral actual (ESP32): {peak.get('threshold_c')} °C")
        if peak.get("enabled"):
            self.threshold_line.setValue(peak.get("threshold_c", 0))
            self.threshold_line.show()
        else:
            self.threshold_line.hide()
        for v in proto.VARIABLES:
            name = proto.FIELDS[v][1]
            if v in variables:
                self.plots[v].setTitle(name)
                self.val_labels[v].setStyleSheet("")
            else:
                self.plots[v].setTitle(f"{name} — no transmitida", color=COLOR_OFF)
                self.val_labels[v].setText("no transmitida")
                self.val_labels[v].setStyleSheet(f"color:{COLOR_OFF}")
        if not avg.get("enabled"):
            self.lbl_avg.setText("inactivo")
            self.lbl_ref.setText("—")
        self.rate.clear()

    def _on_sample(self, msg):
        self.last_sample_pc = time.monotonic()
        seq, t_ms = msg.get("seq"), msg.get("t_ms")
        if self.last_seq is not None and seq is not None and seq > self.last_seq + 1:
            self.lost_packets += seq - self.last_seq - 1
            self.log_msg(f"Se perdieron {seq - self.last_seq - 1} paquetes antes de seq={seq}", COLOR_WARN)
        self.last_seq = seq
        self.rate.add(t_ms)
        t_s = t_ms / 1000.0

        self.lbl_seq.setText(f"seq: {seq}   t_ms: {t_ms}")
        hz = self.rate.hz()
        if hz:
            self.lbl_rate.setText(f"Frecuencia medida: {hz:.2f} Hz (periodo {1000 / hz:.0f} ms)")

        if not msg.get("valid", False):
            self.invalid_count += 1
            for v in proto.VARIABLES:
                if self.confirmed is None or v in self.confirmed.get("variables", []):
                    self.val_labels[v].setText("inválida")
                    self.val_labels[v].setStyleSheet(f"color:{COLOR_ERR}")
            self.lbl_data.setText(f"Datos: lectura inválida ({msg.get('error')})")
            self.lbl_data.setStyleSheet(f"color:{COLOR_ERR}")
        else:
            self.lbl_data.setText("Datos: recibiendo")
            self.lbl_data.setStyleSheet(f"color:{COLOR_OK}")
            for v in proto.VARIABLES:
                field, _, unit = proto.FIELDS[v]
                if field in msg:
                    val = msg[field]
                    self.data[v].append((t_s, val))
                    self.val_labels[v].setText(f"{val:.2f}")
                    self.val_labels[v].setStyleSheet("")
            if "temperature_c" in msg:
                self.ref_avg.add(t_ms, msg["temperature_c"])

        if "temp_avg_c" in msg:
            avg = msg["temp_avg_c"]
            self.avg_data.append((t_s, avg))
            self.lbl_avg.setText(f"{avg:.3f}")
            complete = msg.get("avg_window_complete")
            w = msg.get("avg_window_s")
            self.lbl_avg_info.setText(
                f"Ventana: W={w} s, N={msg.get('avg_n')} muestras, "
                + ("completa" if complete else "<span style='color:%s'>incompleta</span>" % COLOR_WARN))
            ref, n = self.ref_avg.mean(t_ms, w)
            if ref is not None and "temperature_c" in msg:
                self.lbl_ref.setText(f"{ref:.3f} (Δ {avg - ref:+.3f})")
            else:
                self.lbl_ref.setText("requiere T transmitida")

        self._update_counts()
        self._write_csv("sample", msg)
        self._redraw()

    def _on_peak(self, msg):
        self.peak_count += 1
        t_s = msg["t_ms"] / 1000.0
        self.peak_points.append((t_s, msg["peak_c"]))
        row = self.peak_table.rowCount()
        self.peak_table.insertRow(row)
        values = [str(msg.get("peak_seq", self.peak_count)), datetime.now().strftime("%H:%M:%S"),
                  f"{t_s:.3f}", f"{msg['peak_c']:.2f}", f"{msg['threshold_c']:.1f}"]
        for col, val in enumerate(values):
            self.peak_table.setItem(row, col, QTableWidgetItem(val))
        self.peak_table.scrollToBottom()
        self.log_msg(f"Pico detectado: {msg['peak_c']:.2f} °C (θ={msg['threshold_c']:.1f}) en t={t_s:.3f} s",
                     COLOR_OK)
        self._update_counts()
        self._write_csv("peak", msg)
        self._redraw()

    def _update_counts(self):
        self.lbl_counts.setText(f"Picos: {self.peak_count}   Inválidas: {self.invalid_count}   "
                                f"Perdidos: {self.lost_packets}")

    def _redraw(self):
        for v in proto.VARIABLES:
            pts = self.data[v]
            self.curves[v].setData([p[0] for p in pts], [p[1] for p in pts])
        self.avg_curve.setData([p[0] for p in self.avg_data], [p[1] for p in self.avg_data])
        self.peak_scatter.setData([p[0] for p in self.peak_points], [p[1] for p in self.peak_points])

    def _check_data_timeout(self):
        if not self.is_connected() or self.last_sample_pc is None:
            return
        if self.confirmed and not self.confirmed.get("streaming", True):
            self.lbl_data.setText("Datos: transmisión detenida")
            self.lbl_data.setStyleSheet(f"color:{COLOR_OFF}")
            return
        hz = (self.confirmed or {}).get("sampling_hz", 1) or 1
        limit = max(3.0 / hz, 3.0)
        elapsed = time.monotonic() - self.last_sample_pc
        if elapsed > limit:
            self.lbl_data.setText(f"Datos: sin información hace {elapsed:.0f} s")
            self.lbl_data.setStyleSheet(f"color:{COLOR_ERR}; font-weight:bold")

    def clear_data(self):
        for d in self.data.values():
            d.clear()
        self.avg_data.clear()
        self.peak_points.clear()
        self.peak_table.setRowCount(0)
        self.peak_count = self.invalid_count = self.lost_packets = 0
        self._update_counts()
        self._redraw()

    # registro CSV

    def toggle_csv(self, on):
        if on:
            LOG_DIR.mkdir(exist_ok=True)
            path = LOG_DIR / f"sesion_{datetime.now():%Y%m%d_%H%M%S}.csv"
            self.csv_file = open(path, "w", newline="", encoding="utf-8")
            self.csv_writer = csv.DictWriter(self.csv_file, fieldnames=CSV_FIELDS, extrasaction="ignore")
            self.csv_writer.writeheader()
            self.log_msg(f"Guardando en {path}", COLOR_OK)
        elif self.csv_file:
            self.csv_file.close()
            self.log_msg("Archivo CSV cerrado", COLOR_OK)
            self.csv_file = None
            self.csv_writer = None

    def _write_csv(self, kind, msg):
        if not self.csv_writer:
            return
        row = dict(msg)
        row["kind"] = kind
        row["pc_time"] = datetime.now().isoformat(timespec="milliseconds")
        self.csv_writer.writerow(row)
        self.csv_file.flush()

    def closeEvent(self, event):
        self.reconnect_timer.stop()
        if self.worker:
            self.user_disconnect = True
            self.worker.stop()
            self.worker.wait(1000)
        if self.csv_file:
            self.csv_file.close()
        super().closeEvent(event)
