#!/usr/bin/env python3
"""PySide6 GUI for MNIST FC UART inference on the DE2i-150 board."""

from __future__ import annotations

import sys
import time
from pathlib import Path

import pyqtgraph as pg
import serial
from serial.tools import list_ports
from PySide6 import QtCore, QtGui, QtWidgets

from mnist_uart_protocol import (
    CMD_INFER,
    CMD_PING,
    DEFAULT_IMAGE_DIR,
    MnistSample,
    load_pgm_dir,
    load_pgm_sample,
    load_test_vectors,
    read_pgm_pixels,
    send_frame,
    validate_infer_response,
)


class SerialTask(QtCore.QThread):
    ping_done = QtCore.Signal(str)
    infer_done = QtCore.Signal(object, str, bool, object)
    failed = QtCore.Signal(str)

    def __init__(
        self,
        *,
        task: str,
        port: str,
        baud: int,
        timeout_s: float,
        sample: MnistSample | None = None,
        require_label_match: bool = False,
        parent: QtCore.QObject | None = None,
    ) -> None:
        super().__init__(parent)
        self.task = task
        self.port = port
        self.baud = baud
        self.timeout_s = timeout_s
        self.sample = sample
        self.require_label_match = require_label_match

    def run(self) -> None:
        try:
            with serial.Serial(self.port, self.baud, timeout=0.2) as ser:
                time.sleep(0.1)
                ser.reset_input_buffer()
                if self.task == "ping":
                    line = send_frame(ser, CMD_PING, b"", self.timeout_s)
                    self.ping_done.emit(line)
                    return

                if self.sample is None:
                    raise ValueError("no sample selected")
                line = send_frame(ser, CMD_INFER, self.sample.payload, self.timeout_s)
                ok, result = validate_infer_response(
                    line,
                    self.sample,
                    require_label_match=self.require_label_match,
                )
                self.infer_done.emit(self.sample, line, ok, result)
        except Exception as exc:
            self.failed.emit(str(exc))


class ScorePlot(pg.PlotWidget):
    def __init__(self) -> None:
        super().__init__()
        self.setBackground("w")
        self.showGrid(x=True, y=True, alpha=0.25)
        self.setLabel("bottom", "Class")
        self.setLabel("left", "INT8 score")
        self.getPlotItem().setMenuEnabled(False)
        self.getPlotItem().setMouseEnabled(x=False, y=False)
        self.getAxis("bottom").setTicks([[(i, str(i)) for i in range(10)]])
        self.setYRange(-128, 127)

    def set_scores(self, ref_scores: tuple[int, ...], opt_scores: tuple[int, ...]) -> None:
        self.clear()
        classes = list(range(10))
        ref = list(ref_scores)
        opt = list(opt_scores)
        if len(ref) != 10 or len(opt) != 10:
            return

        self.addItem(
            pg.BarGraphItem(
                x=[x - 0.18 for x in classes],
                height=ref,
                width=0.32,
                brush=pg.mkBrush("#2563eb"),
                pen=pg.mkPen("#1d4ed8"),
            )
        )
        self.addItem(
            pg.BarGraphItem(
                x=[x + 0.18 for x in classes],
                height=opt,
                width=0.32,
                brush=pg.mkBrush("#f97316"),
                pen=pg.mkPen("#ea580c"),
            )
        )
        lower = min(-128, min(ref), min(opt)) - 5
        upper = max(127, max(ref), max(opt)) + 5
        self.setYRange(lower, upper)


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("DE2i-150 MNIST UART Inference")
        self.resize(1180, 760)

        self.samples: list[MnistSample] = []
        self.worker: SerialTask | None = None

        central = QtWidgets.QWidget()
        root = QtWidgets.QHBoxLayout(central)
        root.setContentsMargins(10, 10, 10, 10)
        root.setSpacing(12)
        self.setCentralWidget(central)

        root.addWidget(self._build_left_panel(), 0)
        root.addWidget(self._build_right_panel(), 1)

        self.refresh_ports()
        if DEFAULT_IMAGE_DIR.exists():
            self.load_image_dir(DEFAULT_IMAGE_DIR)
        else:
            self.load_fixed_vectors()

    def _build_left_panel(self) -> QtWidgets.QWidget:
        panel = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(panel)
        layout.setSpacing(10)

        serial_group = QtWidgets.QGroupBox("Board UART")
        serial_layout = QtWidgets.QGridLayout(serial_group)
        self.port_combo = QtWidgets.QComboBox()
        self.port_combo.setEditable(True)
        self.baud_spin = QtWidgets.QSpinBox()
        self.baud_spin.setRange(9600, 1000000)
        self.baud_spin.setValue(115200)
        self.timeout_spin = QtWidgets.QDoubleSpinBox()
        self.timeout_spin.setRange(1.0, 60.0)
        self.timeout_spin.setValue(15.0)
        self.timeout_spin.setSuffix(" s")
        refresh_btn = QtWidgets.QPushButton("Refresh")
        refresh_btn.clicked.connect(self.refresh_ports)
        self.ping_btn = QtWidgets.QPushButton("Ping")
        self.ping_btn.clicked.connect(self.ping_board)

        serial_layout.addWidget(QtWidgets.QLabel("Port"), 0, 0)
        serial_layout.addWidget(self.port_combo, 0, 1)
        serial_layout.addWidget(refresh_btn, 0, 2)
        serial_layout.addWidget(QtWidgets.QLabel("Baud"), 1, 0)
        serial_layout.addWidget(self.baud_spin, 1, 1, 1, 2)
        serial_layout.addWidget(QtWidgets.QLabel("Timeout"), 2, 0)
        serial_layout.addWidget(self.timeout_spin, 2, 1, 1, 2)
        serial_layout.addWidget(self.ping_btn, 3, 0, 1, 3)

        input_group = QtWidgets.QGroupBox("Input Set")
        input_layout = QtWidgets.QVBoxLayout(input_group)
        button_row = QtWidgets.QHBoxLayout()
        load_dir_btn = QtWidgets.QPushButton("Default Dir")
        browse_dir_btn = QtWidgets.QPushButton("Browse Dir")
        browse_img_btn = QtWidgets.QPushButton("Add Image")
        fixed_btn = QtWidgets.QPushButton("Fixed Vectors")
        load_dir_btn.clicked.connect(lambda: self.load_image_dir(DEFAULT_IMAGE_DIR))
        browse_dir_btn.clicked.connect(self.browse_image_dir)
        browse_img_btn.clicked.connect(self.browse_image)
        fixed_btn.clicked.connect(self.load_fixed_vectors)
        button_row.addWidget(load_dir_btn)
        button_row.addWidget(browse_dir_btn)
        button_row.addWidget(browse_img_btn)
        button_row.addWidget(fixed_btn)

        self.sample_list = QtWidgets.QListWidget()
        self.sample_list.currentRowChanged.connect(self.update_selected_sample)
        self.sample_count_label = QtWidgets.QLabel("No samples loaded")

        input_layout.addLayout(button_row)
        input_layout.addWidget(self.sample_count_label)
        input_layout.addWidget(self.sample_list, 1)

        run_group = QtWidgets.QGroupBox("Inference")
        run_layout = QtWidgets.QVBoxLayout(run_group)
        self.require_label_check = QtWidgets.QCheckBox("Require true label match")
        self.run_btn = QtWidgets.QPushButton("Run Selected")
        self.run_btn.clicked.connect(self.run_selected)
        run_layout.addWidget(self.require_label_check)
        run_layout.addWidget(self.run_btn)

        layout.addWidget(serial_group)
        layout.addWidget(input_group, 1)
        layout.addWidget(run_group)
        return panel

    def _build_right_panel(self) -> QtWidgets.QWidget:
        panel = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(panel)
        layout.setSpacing(10)

        top = QtWidgets.QHBoxLayout()
        preview_group = QtWidgets.QGroupBox("Preview")
        preview_layout = QtWidgets.QVBoxLayout(preview_group)
        self.preview_label = QtWidgets.QLabel()
        self.preview_label.setFixedSize(300, 300)
        self.preview_label.setAlignment(QtCore.Qt.AlignmentFlag.AlignCenter)
        self.preview_label.setStyleSheet("background:#111827; color:#e5e7eb;")
        self.sample_info_label = QtWidgets.QLabel("Select a sample")
        self.sample_info_label.setWordWrap(True)
        preview_layout.addWidget(self.preview_label, alignment=QtCore.Qt.AlignmentFlag.AlignCenter)
        preview_layout.addWidget(self.sample_info_label)

        result_group = QtWidgets.QGroupBox("Result")
        result_layout = QtWidgets.QFormLayout(result_group)
        self.status_label = QtWidgets.QLabel("Idle")
        self.ref_cls_label = QtWidgets.QLabel("-")
        self.opt_cls_label = QtWidgets.QLabel("-")
        self.label_match_label = QtWidgets.QLabel("-")
        self.expected_match_label = QtWidgets.QLabel("-")
        self.mismatch_label = QtWidgets.QLabel("-")
        self.ref_cycles_label = QtWidgets.QLabel("-")
        self.opt_cycles_label = QtWidgets.QLabel("-")
        self.speedup_label = QtWidgets.QLabel("-")
        result_layout.addRow("Status", self.status_label)
        result_layout.addRow("Ref class", self.ref_cls_label)
        result_layout.addRow("Opt class", self.opt_cls_label)
        result_layout.addRow("True label match", self.label_match_label)
        result_layout.addRow("Expected match", self.expected_match_label)
        result_layout.addRow("Score mismatches", self.mismatch_label)
        result_layout.addRow("Ref cycles", self.ref_cycles_label)
        result_layout.addRow("Opt cycles", self.opt_cycles_label)
        result_layout.addRow("Speedup", self.speedup_label)

        top.addWidget(preview_group, 0)
        top.addWidget(result_group, 1)

        plot_group = QtWidgets.QGroupBox("Class Scores")
        plot_layout = QtWidgets.QVBoxLayout(plot_group)
        legend = QtWidgets.QLabel("Blue: TFLM reference    Orange: PULP optimized")
        self.score_plot = ScorePlot()
        plot_layout.addWidget(legend)
        plot_layout.addWidget(self.score_plot, 1)

        log_group = QtWidgets.QGroupBox("UART Log")
        log_layout = QtWidgets.QVBoxLayout(log_group)
        self.log_text = QtWidgets.QPlainTextEdit()
        self.log_text.setReadOnly(True)
        self.log_text.setMaximumBlockCount(500)
        log_layout.addWidget(self.log_text)

        layout.addLayout(top)
        layout.addWidget(plot_group, 1)
        layout.addWidget(log_group, 1)
        return panel

    def refresh_ports(self) -> None:
        current = self.current_port()
        self.port_combo.clear()
        ports = list(list_ports.comports())
        for item in ports:
            label = f"{item.device}  {item.description}"
            self.port_combo.addItem(label, item.device)
        if not ports:
            self.port_combo.addItem("/dev/ttyUSB0", "/dev/ttyUSB0")
        if current:
            index = self.port_combo.findData(current)
            if index >= 0:
                self.port_combo.setCurrentIndex(index)

    def current_port(self) -> str:
        data = self.port_combo.currentData()
        if data:
            return str(data)
        text = self.port_combo.currentText().strip()
        if not text:
            return "/dev/ttyUSB0"
        return text.split()[0]

    def set_busy(self, busy: bool) -> None:
        self.ping_btn.setEnabled(not busy)
        self.run_btn.setEnabled(not busy)
        self.sample_list.setEnabled(not busy)

    def append_log(self, text: str) -> None:
        self.log_text.appendPlainText(text)

    def load_image_dir(self, directory: Path) -> None:
        try:
            self.samples = load_pgm_dir(directory)
            self.populate_samples(f"Loaded {len(self.samples)} images from {directory}")
        except Exception as exc:
            QtWidgets.QMessageBox.warning(self, "Load images", str(exc))

    def browse_image_dir(self) -> None:
        selected = QtWidgets.QFileDialog.getExistingDirectory(
            self,
            "Select PGM image directory",
            str(DEFAULT_IMAGE_DIR),
        )
        if selected:
            self.load_image_dir(Path(selected))

    def browse_image(self) -> None:
        selected, _ = QtWidgets.QFileDialog.getOpenFileName(
            self,
            "Select PGM image",
            str(DEFAULT_IMAGE_DIR),
            "PGM images (*.pgm);;All files (*)",
        )
        if not selected:
            return
        try:
            sample = load_pgm_sample(Path(selected))
            self.samples.append(sample)
            self.populate_samples(f"Added {sample.name}")
            self.sample_list.setCurrentRow(len(self.samples) - 1)
        except Exception as exc:
            QtWidgets.QMessageBox.warning(self, "Load image", str(exc))

    def load_fixed_vectors(self) -> None:
        try:
            self.samples = load_test_vectors()
            self.populate_samples(f"Loaded {len(self.samples)} fixed vectors")
        except Exception as exc:
            QtWidgets.QMessageBox.warning(self, "Load fixed vectors", str(exc))

    def populate_samples(self, message: str) -> None:
        self.sample_list.clear()
        for sample in self.samples:
            label = "?" if sample.label is None else str(sample.label)
            self.sample_list.addItem(f"{sample.name}    label={label}")
        self.sample_count_label.setText(f"{len(self.samples)} sample(s)")
        self.append_log(message)
        if self.samples:
            self.sample_list.setCurrentRow(0)

    def selected_sample(self) -> MnistSample | None:
        row = self.sample_list.currentRow()
        if row < 0 or row >= len(self.samples):
            return None
        return self.samples[row]

    def update_selected_sample(self, _row: int) -> None:
        sample = self.selected_sample()
        if sample is None:
            self.preview_label.setText("No sample")
            self.sample_info_label.setText("Select a sample")
            return
        self.show_preview(sample)
        label = "unknown" if sample.label is None else str(sample.label)
        source = "" if sample.source is None else f"\n{sample.source}"
        self.sample_info_label.setText(f"{sample.name}\nLabel: {label}{source}")

    def show_preview(self, sample: MnistSample) -> None:
        pixels = self.preview_pixels(sample)
        image = QtGui.QImage(
            pixels,
            28,
            28,
            28,
            QtGui.QImage.Format.Format_Grayscale8,
        ).copy()
        pixmap = QtGui.QPixmap.fromImage(image).scaled(
            self.preview_label.size(),
            QtCore.Qt.AspectRatioMode.KeepAspectRatio,
            QtCore.Qt.TransformationMode.FastTransformation,
        )
        self.preview_label.setPixmap(pixmap)

    def preview_pixels(self, sample: MnistSample) -> bytes:
        if sample.source and Path(sample.source).suffix.lower() == ".pgm":
            try:
                return read_pgm_pixels(Path(sample.source))
            except Exception:
                pass
        return bytes((byte + 128) & 0xFF for byte in sample.payload)

    def ping_board(self) -> None:
        self.start_worker("ping")

    def run_selected(self) -> None:
        sample = self.selected_sample()
        if sample is None:
            QtWidgets.QMessageBox.information(self, "Run inference", "Select a sample first.")
            return
        self.start_worker("infer", sample)

    def start_worker(self, task: str, sample: MnistSample | None = None) -> None:
        if self.worker is not None and self.worker.isRunning():
            return
        self.set_busy(True)
        self.status_label.setText("Running")
        self.status_label.setStyleSheet("")
        self.worker = SerialTask(
            task=task,
            port=self.current_port(),
            baud=self.baud_spin.value(),
            timeout_s=self.timeout_spin.value(),
            sample=sample,
            require_label_match=self.require_label_check.isChecked(),
        )
        self.worker.ping_done.connect(self.on_ping_done)
        self.worker.infer_done.connect(self.on_infer_done)
        self.worker.failed.connect(self.on_worker_failed)
        self.worker.finished.connect(lambda: self.set_busy(False))
        self.worker.start()

    def on_ping_done(self, line: str) -> None:
        self.status_label.setText("Ping OK" if line.startswith("OK ") else "Ping failed")
        self.status_label.setStyleSheet(
            "color:#15803d; font-weight:600;"
            if line.startswith("OK ")
            else "color:#b91c1c; font-weight:600;"
        )
        self.append_log(line)

    def on_infer_done(
        self,
        sample: MnistSample,
        line: str,
        ok: bool,
        result: dict[str, object],
    ) -> None:
        self.append_log(line)
        self.status_label.setText("PASS" if ok else "FAIL")
        self.status_label.setStyleSheet(
            "color:#15803d; font-weight:600;" if ok else "color:#b91c1c; font-weight:600;"
        )
        self.ref_cls_label.setText(str(result.get("ref_cls", "-")))
        self.opt_cls_label.setText(str(result.get("opt_cls", "-")))
        self.label_match_label.setText(self.format_label_match(sample, result))
        self.expected_match_label.setText(self.format_expected_match(sample, result))
        self.mismatch_label.setText(str(result.get("mismatches", "-")))

        ref_cycles = int(result.get("ref_cycles", 0))
        opt_cycles = int(result.get("opt_cycles", 0))
        self.ref_cycles_label.setText(str(ref_cycles))
        self.opt_cycles_label.setText(str(opt_cycles))
        self.speedup_label.setText(f"{ref_cycles / opt_cycles:.2f}x" if opt_cycles else "-")

        ref_scores = result.get("ref_scores")
        opt_scores = result.get("opt_scores")
        if isinstance(ref_scores, tuple) and isinstance(opt_scores, tuple):
            self.score_plot.set_scores(ref_scores, opt_scores)

    def format_label_match(self, sample: MnistSample, result: dict[str, object]) -> str:
        if sample.label is None:
            return "unknown"
        return "yes" if result.get("label_match") is True else "no"

    def format_expected_match(self, sample: MnistSample, result: dict[str, object]) -> str:
        if sample.expected_class is None:
            return "n/a"
        return "yes" if result.get("expected_match") is True else "no"

    def on_worker_failed(self, message: str) -> None:
        self.status_label.setText("Error")
        self.status_label.setStyleSheet("color:#b91c1c; font-weight:600;")
        self.append_log(f"ERROR {message}")
        QtWidgets.QMessageBox.warning(self, "UART operation failed", message)


def main() -> int:
    app = QtWidgets.QApplication(sys.argv)
    app.setStyle("Fusion")
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
