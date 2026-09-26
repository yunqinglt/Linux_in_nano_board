"""PyQt6 desktop frontend."""

from __future__ import annotations

from datetime import datetime
from pathlib import Path
import sys
import traceback
from typing import Callable

from PyQt6.QtCore import QObject, QThread, pyqtSignal, pyqtSlot
from PyQt6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QFileDialog,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from .device import MaintenanceDevice
from .image import IMAGE_SIZE, build_image
from .operations import (
    CancellationToken,
    OperationCancelled,
    flash_image,
    readback_image,
    test_chip_integrity,
    verify_image,
)
from .protocol import DEFAULT_PID, DEFAULT_VID
from .transport import UsbBulkTransport, UsbDeviceInfo, enumerate_devices


REPOSITORY = Path(__file__).resolve().parents[2]
DEFAULT_RESOURCES = REPOSITORY / "res"
DEFAULT_IMAGE = REPOSITORY / "host" / "out" / "mipsel-emu-8m.img"


class Worker(QObject):
    progress = pyqtSignal(int, str)
    succeeded = pyqtSignal(str)
    failed = pyqtSignal(str)
    cancelled = pyqtSignal(str)
    finished = pyqtSignal()

    def __init__(
        self,
        operation: Callable[[CancellationToken, Callable[[int, int, str], None]], str],
        token: CancellationToken,
    ) -> None:
        super().__init__()
        self.operation = operation
        self.token = token

    def report(self, completed: int, total: int, message: str) -> None:
        percent = min(100, int(completed * 100 / total) if total else 100)
        self.progress.emit(percent, message)

    @pyqtSlot()
    def run(self) -> None:
        try:
            self.succeeded.emit(self.operation(self.token, self.report))
        except OperationCancelled as error:
            self.cancelled.emit(str(error))
        except Exception as error:
            self.failed.emit(f"{error}\n\n{traceback.format_exc()}")
        finally:
            self.finished.emit()


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("CH32V203 MIPS Image Tool")
        self.resize(850, 620)
        self._thread: QThread | None = None
        self._worker: Worker | None = None
        self._token: CancellationToken | None = None
        self._busy_controls: list[QPushButton] = []
        self._build_ui()
        self.refresh_devices()

    def _build_ui(self) -> None:
        root = QWidget(self)
        layout = QVBoxLayout(root)

        device_group = QGroupBox("USB Vendor device")
        device_layout = QGridLayout(device_group)
        self.device_combo = QComboBox()
        self.refresh_button = QPushButton("Refresh")
        self.refresh_button.clicked.connect(self.refresh_devices)
        self.info_button = QPushButton("Read info")
        self.info_button.clicked.connect(self.read_info)
        device_layout.addWidget(QLabel("1209:2036"), 0, 0)
        device_layout.addWidget(self.device_combo, 0, 1)
        device_layout.addWidget(self.refresh_button, 0, 2)
        device_layout.addWidget(self.info_button, 0, 3)
        layout.addWidget(device_group)

        image_group = QGroupBox("Fixed 8 MiB W25Q64 image")
        image_layout = QGridLayout(image_group)
        self.resource_edit = QLineEdit(str(DEFAULT_RESOURCES))
        resource_browse = QPushButton("Browse…")
        resource_browse.clicked.connect(self.browse_resources)
        self.image_edit = QLineEdit(str(DEFAULT_IMAGE))
        image_browse = QPushButton("Browse…")
        image_browse.clicked.connect(self.browse_image)
        self.pack_button = QPushButton("Pack resources")
        self.pack_button.clicked.connect(self.pack_resources)
        image_layout.addWidget(QLabel("res directory"), 0, 0)
        image_layout.addWidget(self.resource_edit, 0, 1)
        image_layout.addWidget(resource_browse, 0, 2)
        image_layout.addWidget(QLabel("image file"), 1, 0)
        image_layout.addWidget(self.image_edit, 1, 1)
        image_layout.addWidget(image_browse, 1, 2)
        image_layout.addWidget(self.pack_button, 2, 2)
        layout.addWidget(image_group)

        actions_group = QGroupBox("Hardware operations")
        actions_layout = QGridLayout(actions_group)
        self.test_flash_button = QPushButton("Test W25Q64")
        self.test_flash_button.clicked.connect(lambda: self.test_chip("flash"))
        self.test_psram_button = QPushButton("Test APS6404L")
        self.test_psram_button.clicked.connect(lambda: self.test_chip("psram"))
        self.full_test = QCheckBox("Full-chip test (quick mode samples 5 blocks)")
        self.flash_button = QPushButton("Flash + readback verify")
        self.flash_button.clicked.connect(self.flash)
        self.verify_button = QPushButton("Verify existing flash")
        self.verify_button.clicked.connect(self.verify)
        self.readback_button = QPushButton("Save readback…")
        self.readback_button.clicked.connect(self.readback)
        actions_layout.addWidget(self.test_flash_button, 0, 0)
        actions_layout.addWidget(self.test_psram_button, 0, 1)
        actions_layout.addWidget(self.full_test, 0, 2)
        actions_layout.addWidget(self.flash_button, 1, 0)
        actions_layout.addWidget(self.verify_button, 1, 1)
        actions_layout.addWidget(self.readback_button, 1, 2)
        layout.addWidget(actions_group)

        progress_layout = QHBoxLayout()
        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        self.cancel_button = QPushButton("Cancel")
        self.cancel_button.setEnabled(False)
        self.cancel_button.clicked.connect(self.cancel_operation)
        progress_layout.addWidget(self.progress)
        progress_layout.addWidget(self.cancel_button)
        layout.addLayout(progress_layout)

        self.status_label = QLabel("Ready")
        layout.addWidget(self.status_label)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        layout.addWidget(self.log, 1)
        self.setCentralWidget(root)

        self._busy_controls = [
            self.refresh_button,
            self.info_button,
            self.pack_button,
            self.test_flash_button,
            self.test_psram_button,
            self.flash_button,
            self.verify_button,
            self.readback_button,
        ]

    def append_log(self, message: str) -> None:
        timestamp = datetime.now().strftime("%H:%M:%S")
        self.log.appendPlainText(f"[{timestamp}] {message}")

    def selected_device(self) -> UsbDeviceInfo:
        info = self.device_combo.currentData()
        if not isinstance(info, UsbDeviceInfo):
            raise RuntimeError("no maintenance device selected")
        return info

    @pyqtSlot()
    def refresh_devices(self) -> None:
        self.device_combo.clear()
        try:
            devices = enumerate_devices()
            for info in devices:
                self.device_combo.addItem(info.label, info)
            if not devices:
                self.device_combo.addItem("No 1209:2036 device detected", None)
                self.append_log("No USB vendor device detected.")
            else:
                self.append_log(f"Detected {len(devices)} device(s).")
        except Exception as error:
            self.device_combo.addItem("USB backend unavailable", None)
            self._show_error(str(error))

    def browse_resources(self) -> None:
        path = QFileDialog.getExistingDirectory(
            self, "Select res directory", self.resource_edit.text()
        )
        if path:
            self.resource_edit.setText(path)

    def browse_image(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Select 8 MiB image", self.image_edit.text(), "Images (*.img *.bin);;All (*)"
        )
        if path:
            self.image_edit.setText(path)

    def _device_operation(
        self, action: Callable[[MaintenanceDevice, CancellationToken, Callable], str]
    ) -> Callable:
        info = self.selected_device()

        def wrapped(token: CancellationToken, progress: Callable) -> str:
            with UsbBulkTransport.open(info) as transport:
                device = MaintenanceDevice(transport)
                return action(device, token, progress)

        return wrapped

    def _start(self, title: str, operation: Callable) -> None:
        if self._thread is not None:
            return
        self.append_log(title)
        self.status_label.setText(title)
        self.progress.setValue(0)
        self._token = CancellationToken()
        self._thread = QThread(self)
        self._worker = Worker(operation, self._token)
        self._worker.moveToThread(self._thread)
        self._thread.started.connect(self._worker.run)
        self._worker.progress.connect(self._on_progress)
        self._worker.succeeded.connect(self._on_succeeded)
        self._worker.failed.connect(self._on_failed)
        self._worker.cancelled.connect(self._on_cancelled)
        self._worker.finished.connect(self._thread.quit)
        self._worker.finished.connect(self._worker.deleteLater)
        self._thread.finished.connect(self._thread.deleteLater)
        self._thread.finished.connect(self._operation_finished)
        for control in self._busy_controls:
            control.setEnabled(False)
        self.cancel_button.setEnabled(True)
        self._thread.start()

    @pyqtSlot(int, str)
    def _on_progress(self, percent: int, message: str) -> None:
        self.progress.setValue(percent)
        self.status_label.setText(message)

    @pyqtSlot(str)
    def _on_succeeded(self, message: str) -> None:
        self.progress.setValue(100)
        self.status_label.setText(message)
        self.append_log(message)

    @pyqtSlot(str)
    def _on_cancelled(self, message: str) -> None:
        self.status_label.setText(message)
        self.append_log(message)

    @pyqtSlot(str)
    def _on_failed(self, detail: str) -> None:
        summary = detail.splitlines()[0]
        self.status_label.setText(summary)
        self.append_log(summary)
        self._show_error(detail)

    @pyqtSlot()
    def _operation_finished(self) -> None:
        self._thread = None
        self._worker = None
        self._token = None
        for control in self._busy_controls:
            control.setEnabled(True)
        self.cancel_button.setEnabled(False)

    @pyqtSlot()
    def cancel_operation(self) -> None:
        if self._token:
            self._token.cancel()
            self.cancel_button.setEnabled(False)
            self.status_label.setText("Cancellation requested; restoring current block…")

    def _show_error(self, message: str) -> None:
        QMessageBox.critical(self, "CH32 image tool", message)

    def read_info(self) -> None:
        def action(device: MaintenanceDevice, _token: CancellationToken, _progress: Callable) -> str:
            info = device.get_info()
            return (
                f"Firmware {info.firmware_version}; W25Q64 {info.flash_size:,} B "
                f"ID {info.flash_id_text}; APS6404L {info.psram_size:,} B "
                f"ID {info.psram_id_text}; transfer {info.max_transfer} B"
            )

        try:
            self._start("Reading device information…", self._device_operation(action))
        except Exception as error:
            self._show_error(str(error))

    def pack_resources(self) -> None:
        resources = Path(self.resource_edit.text())
        output = Path(self.image_edit.text())

        def action(_token: CancellationToken, progress: Callable) -> str:
            manifest = build_image(resources, output, progress)
            return (
                f"Packed {output} ({IMAGE_SIZE:,} bytes, "
                f"CRC32 {manifest.image_crc32:08X})"
            )

        self._start("Packing fixed image…", action)

    def flash(self) -> None:
        image = Path(self.image_edit.text())
        answer = QMessageBox.question(
            self,
            "Erase W25Q64?",
            "This will erase the complete W25Q64, program the selected 8 MiB "
            "image, then read every byte back. Continue?",
        )
        if answer != QMessageBox.StandardButton.Yes:
            return

        def action(device: MaintenanceDevice, token: CancellationToken, progress: Callable) -> str:
            result = flash_image(device, image, token=token, progress=progress)
            return f"Flash and readback verification passed; CRC32 {result.crc32:08X}."

        try:
            self._start("Erasing and programming W25Q64…", self._device_operation(action))
        except Exception as error:
            self._show_error(str(error))

    def verify(self) -> None:
        image = Path(self.image_edit.text())

        def action(device: MaintenanceDevice, token: CancellationToken, progress: Callable) -> str:
            result = verify_image(device, image, token=token, progress=progress)
            return f"All {result.bytes_processed:,} flash bytes match the image."

        try:
            self._start("Reading back and comparing W25Q64…", self._device_operation(action))
        except Exception as error:
            self._show_error(str(error))

    def readback(self) -> None:
        suggested = str(Path(self.image_edit.text()).with_name("w25q64-readback.img"))
        path, _ = QFileDialog.getSaveFileName(
            self, "Save W25Q64 readback", suggested, "Images (*.img *.bin);;All (*)"
        )
        if not path:
            return

        def action(device: MaintenanceDevice, token: CancellationToken, progress: Callable) -> str:
            result = readback_image(device, path, token=token, progress=progress)
            return f"Saved {result.bytes_processed:,} bytes; CRC32 {result.crc32:08X}."

        try:
            self._start("Reading complete W25Q64…", self._device_operation(action))
        except Exception as error:
            self._show_error(str(error))

    def test_chip(self, chip: str) -> None:
        full = self.full_test.isChecked()
        if full and chip == "flash":
            answer = QMessageBox.warning(
                self,
                "Flash endurance warning",
                "A full W25Q64 test erases, patterns, and restores every sector. "
                "It preserves content but consumes erase endurance. Continue?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if answer != QMessageBox.StandardButton.Yes:
                return

        def action(device: MaintenanceDevice, token: CancellationToken, progress: Callable) -> str:
            result = test_chip_integrity(
                device, chip, full=full, token=token, progress=progress
            )
            return (
                f"{chip} integrity test passed for {result.blocks_tested} blocks; "
                "original data restored."
            )

        try:
            self._start(f"Testing {chip} integrity…", self._device_operation(action))
        except Exception as error:
            self._show_error(str(error))

    def closeEvent(self, event) -> None:  # type: ignore[no-untyped-def]
        if self._thread is not None:
            QMessageBox.information(
                self,
                "Operation active",
                "Cancel the active operation and wait for the current block to be restored.",
            )
            event.ignore()
            return
        event.accept()


def main() -> int:
    application = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    return application.exec()


if __name__ == "__main__":
    raise SystemExit(main())
