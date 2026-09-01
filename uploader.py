import sys
import os
import random
import json
import hashlib
import shutil
import subprocess
import configparser
import re
from pathlib import Path

import requests
from PyQt5.QtWidgets import (
	QApplication,
	QWidget,
	QVBoxLayout,
	QComboBox,
	QPushButton,
	QTextEdit,
	QLineEdit,
	QFormLayout,
	QCheckBox,
)
from PyQt5.QtCore import QThread, pyqtSignal

PROJECT_DIR = Path(__file__).resolve().parent
UID_FILE = PROJECT_DIR / "used_uids.json"
MAINBOARD_CONFIG_FILE = PROJECT_DIR / "uploader.conf"
GENERATED_API_CA_CERT_HEADER = PROJECT_DIR / "src" / "GeneratedApiCaCert.h"


def resolve_project_path(value):
	path = Path(value).expanduser()
	return path if path.is_absolute() else PROJECT_DIR / path


def read_api_ca_cert(cfg):
	certificate_file = cfg.get("api_ca_cert_file", "").strip()
	if not certificate_file:
		if cfg["server_endpoint"].lower().startswith("https://"):
			raise ValueError(
				"API CA certificate file is required when server_endpoint uses HTTPS"
			)
		return ""

	certificate_path = resolve_project_path(certificate_file)
	try:
		certificate = certificate_path.read_text(encoding="utf-8").strip()
	except OSError as exc:
		raise ValueError(f"Could not read API CA certificate: {certificate_path}") from exc
	if "-----BEGIN CERTIFICATE-----" not in certificate or \
			"-----END CERTIFICATE-----" not in certificate:
		raise ValueError(f"API CA certificate is not a PEM certificate: {certificate_path}")
	return certificate


def update_firmware_manifest(manifest_path, version, channel, download_url, firmware_path, notes_url, config_schema):
	if not download_url.startswith("https://"):
		raise ValueError("Firmware download URL must use HTTPS")
	if channel not in ("stable", "prerelease"):
		raise ValueError("Firmware channel must be stable or prerelease")
	if not firmware_path.is_file():
		raise FileNotFoundError(f"Firmware image not found: {firmware_path}")

	digest = hashlib.sha256()
	with firmware_path.open("rb") as firmware:
		first_byte = firmware.read(1)
		digest.update(first_byte)
		for chunk in iter(lambda: firmware.read(1024 * 1024), b""):
			digest.update(chunk)
	sha256 = digest.hexdigest()
	if first_byte != b"\xe9":
		raise ValueError("Generated file is not an ESP32 firmware image")

	manifest = {"releases": []}
	if manifest_path.is_file():
		with manifest_path.open("r", encoding="utf-8") as manifest_file:
			manifest = json.load(manifest_file)
		if not isinstance(manifest, dict) or not isinstance(manifest.get("releases"), list):
			raise ValueError("Existing manifest must contain a releases array")

	release = {
		"version": version,
		"channel": channel,
		"config_schema": int(config_schema),
		"asset": {
			"url": download_url,
			"sha256": sha256,
			"size": firmware_path.stat().st_size,
		},
	}
	if notes_url:
		release["notes_url"] = notes_url

	manifest["releases"] = [
		item for item in manifest["releases"]
		if not isinstance(item, dict) or item.get("version") != version
	]
	manifest["releases"].append(release)
	manifest_path.parent.mkdir(parents=True, exist_ok=True)
	temporary_path = manifest_path.with_suffix(manifest_path.suffix + ".tmp")
	with temporary_path.open("w", encoding="utf-8") as manifest_file:
		json.dump(manifest, manifest_file, indent=2)
		manifest_file.write("\n")
	temporary_path.replace(manifest_path)
	return sha256, firmware_path.stat().st_size

class PioUploadThread(QThread):
	log_signal = pyqtSignal(str)
	finished_signal = pyqtSignal(bool)

	def __init__(self, env_name, build_flags, port=None):
		super().__init__()
		self.env_name = env_name
		self.build_flags = build_flags
		self.port = port

	def run(self):
		env = os.environ.copy()
		# An empty override would hide the environment's normal build flags.
		# Substations do not need per-device defines, so leave the PlatformIO
		# environment unchanged for that target.
		if self.build_flags:
			env["PLATFORMIO_BUILD_FLAGS"] = self.build_flags
		else:
			env.pop("PLATFORMIO_BUILD_FLAGS", None)

		self.log_signal.emit(f"--- Building and uploading {self.env_name} over serial ---")

		try:
			command = ["pio", "run", "-e", self.env_name, "-t", "upload"]
			if self.port:
				command.extend(["--upload-port", self.port])
			process = subprocess.Popen(
				command,
				cwd=PROJECT_DIR,
				env=env,
				stdout=subprocess.PIPE,
				stderr=subprocess.STDOUT,
				text=True
			)

			for line in process.stdout:
				self.log_signal.emit(line.strip())

			process.wait()
			self.finished_signal.emit(process.returncode == 0)

		except FileNotFoundError:
			self.log_signal.emit("ERROR: 'pio' command not found. Ensure PlatformIO is in your system PATH.")
			self.finished_signal.emit(False)
		except Exception as e:
			self.log_signal.emit(f"ERROR: {str(e)}")
			self.finished_signal.emit(False)

class PioOtaThread(QThread):
	log_signal = pyqtSignal(str)
	finished_signal = pyqtSignal(bool)

	def __init__(self, env_name, build_flags, host, port, password):
		super().__init__()
		self.env_name = env_name
		self.build_flags = build_flags
		self.host = host
		self.port = port
		self.password = password

	def run(self):
		env = os.environ.copy()
		env["PLATFORMIO_BUILD_FLAGS"] = self.build_flags

		try:
			self.log_signal.emit(f"--- Building {self.env_name} for Wi-Fi OTA ---")
			process = subprocess.Popen(
				["pio", "run", "-e", self.env_name],
				cwd=PROJECT_DIR,
				env=env,
				stdout=subprocess.PIPE,
				stderr=subprocess.STDOUT,
				text=True,
			)

			for line in process.stdout:
				self.log_signal.emit(line.rstrip())

			process.wait()
			if process.returncode != 0:
				self.finished_signal.emit(False)
				return

			firmware_path = PROJECT_DIR / ".pio" / "build" / self.env_name / "firmware.bin"
			if not firmware_path.is_file():
				self.log_signal.emit(f"ERROR: Firmware image not found: {firmware_path}")
				self.finished_signal.emit(False)
				return

			url = f"http://{self.host}:{self.port}/ota/firmware"
			self.log_signal.emit(
				f"--- Uploading {firmware_path.stat().st_size} bytes to {self.host}:{self.port} ---"
			)

			with firmware_path.open("rb") as firmware:
				response = requests.post(
					url,
					headers={"X-OTA-Password": self.password},
					files={"firmware": ("firmware.bin", firmware, "application/octet-stream")},
					timeout=(10, 180),
				)

			if response.ok:
				self.log_signal.emit(f"OTA accepted by board: {response.text}")
				self.finished_signal.emit(True)
			else:
				self.log_signal.emit(f"ERROR: OTA failed with HTTP {response.status_code}: {response.text}")
				self.finished_signal.emit(False)

		except FileNotFoundError:
			self.log_signal.emit("ERROR: 'pio' command not found. Ensure PlatformIO is in your system PATH.")
			self.finished_signal.emit(False)
		except requests.RequestException as exc:
			self.log_signal.emit(f"ERROR: OTA connection failed: {exc}")
			self.finished_signal.emit(False)
		except Exception as exc:
			self.log_signal.emit(f"ERROR: {exc}")
			self.finished_signal.emit(False)


class PioArtifactThread(QThread):
	log_signal = pyqtSignal(str)
	finished_signal = pyqtSignal(bool)

	def __init__(self, build_flags, version, channel, download_url, artifact_dir, manifest_path, notes_url, config_schema):
		super().__init__()
		self.build_flags = build_flags
		self.version = version
		self.channel = channel
		self.download_url = download_url
		self.artifact_dir = artifact_dir
		self.manifest_path = manifest_path
		self.notes_url = notes_url
		self.config_schema = config_schema

	def run(self):
		env = os.environ.copy()
		env["PLATFORMIO_BUILD_FLAGS"] = self.build_flags
		try:
			self.log_signal.emit("--- Building mainboard firmware artifact ---")
			try:
				process = subprocess.Popen(
					["pio", "run", "-e", "mainboard"],
					cwd=PROJECT_DIR,
					env=env,
					stdout=subprocess.PIPE,
					stderr=subprocess.STDOUT,
					text=True,
				)
			except FileNotFoundError:
				self.log_signal.emit("ERROR: 'pio' command not found. Ensure PlatformIO is in your system PATH.")
				self.finished_signal.emit(False)
				return
			for line in process.stdout:
				self.log_signal.emit(line.rstrip())
			process.wait()
			if process.returncode != 0:
				self.finished_signal.emit(False)
				return

			built_firmware = PROJECT_DIR / ".pio" / "build" / "mainboard" / "firmware.bin"
			if not built_firmware.is_file():
				raise FileNotFoundError(f"Firmware image not found: {built_firmware}")
			self.artifact_dir.mkdir(parents=True, exist_ok=True)
			artifact_path = self.artifact_dir / "mb_firmware.bin"
			shutil.copy2(built_firmware, artifact_path)
			sha256, size = update_firmware_manifest(
				self.manifest_path,
				self.version,
				self.channel,
				self.download_url,
				artifact_path,
				self.notes_url,
				self.config_schema,
			)
			self.log_signal.emit(f"Artifact written: {artifact_path} ({size} bytes)")
			self.log_signal.emit(f"Manifest written: {self.manifest_path}")
			self.log_signal.emit(f"SHA-256: {sha256}")
			self.finished_signal.emit(True)
		except FileNotFoundError as exc:
			self.log_signal.emit(f"ERROR: {exc}")
			self.finished_signal.emit(False)
		except Exception as exc:
			self.log_signal.emit(f"ERROR: {exc}")
			self.finished_signal.emit(False)

class PioMonitorThread(QThread):
	log_signal = pyqtSignal(str)

	def __init__(self, env_name, port=None):
		super().__init__()
		self.env_name = env_name
		self.port = port
		self.process = None
		self._stop_requested = False

	def run(self):
		try:
			self.log_signal.emit(f"--- Starting serial monitor for {self.env_name} ---")
			ansi_escape = re.compile(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")
			command = ["pio", "device", "monitor", "-e", self.env_name]
			if self.port:
				command.extend(["--port", self.port])
			self.process = subprocess.Popen(
				command,
				cwd=PROJECT_DIR,
				stdout=subprocess.PIPE,
				stderr=subprocess.STDOUT,
				text=True
			)

			for line in self.process.stdout:
				if self._stop_requested:
					break
				self.log_signal.emit(ansi_escape.sub("", line.rstrip()))

			if self.process:
				self.process.wait()
		except FileNotFoundError:
			self.log_signal.emit("ERROR: 'pio' command not found. Ensure PlatformIO is in your system PATH.")
		except Exception as e:
			self.log_signal.emit(f"ERROR: {str(e)}")

	def stop(self):
		self._stop_requested = True
		if self.process and self.process.poll() is None:
			self.process.terminate()
			try:
				self.process.wait(timeout=2)
			except subprocess.TimeoutExpired:
				self.process.kill()

class WifiLogThread(QThread):
	log_signal = pyqtSignal(str)

	def __init__(self, host, port, password):
		super().__init__()
		self.host = host
		self.port = port
		self.password = password
		self._stop_requested = False
		self._session = None

	def run(self):
		log_url = f"http://{self.host}:{self.port}/ota/log"
		status_url = f"http://{self.host}:{self.port}/ota/status"
		headers = {
			"X-OTA-Password": self.password,
			"Connection": "close",
		}
		cursor = 0
		pending = ""
		connection_error_reported = False
		ansi_escape = re.compile(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")

		self.log_signal.emit(f"--- Starting Wi-Fi log from {self.host}:{self.port} ---")

		with requests.Session() as session:
			self._session = session
			while not self._stop_requested:
				try:
					# Check the cursor using the non-empty status response. Calling
					# /ota/log when nothing changed used to make WebServer emit a
					# warning, which was then captured as the next remote log line.
					status_response = session.get(
						status_url,
						headers=headers,
						timeout=(1, 2.5),
					)
					status_response.raise_for_status()
					board_cursor = int(status_response.json().get("log_cursor", cursor))

					if board_cursor != cursor:
						response = session.get(
							log_url,
							headers=headers,
							params={"since": cursor},
							timeout=(1, 2.5),
						)
						if response.status_code == 401:
							self.log_signal.emit("ERROR: Wi-Fi log authentication failed.")
							break
						response.raise_for_status()

						dropped = response.headers.get("X-Log-Dropped") == "1"
						chunk_start = int(response.headers.get("X-Log-Start", cursor))
						cursor = int(response.headers.get("X-Log-Cursor", cursor))
						is_empty = response.headers.get("X-Log-Empty") == "1"

						text_chunk = "" if is_empty else ansi_escape.sub("", response.text)
						if dropped:
							self.log_signal.emit(
								"--- Board rebooted or older log messages were overwritten ---"
							)
							pending = ""
							# A wrapped ring buffer can begin in the middle of a line.
							if chunk_start > 0:
								newline = text_chunk.find("\n")
								text_chunk = text_chunk[newline + 1:] if newline >= 0 else ""

						pending += text_chunk
						lines = pending.splitlines(keepends=True)
						pending = ""
						for line in lines:
							if line.endswith(("\n", "\r")):
								self.log_signal.emit(line.rstrip("\r\n"))
							else:
								pending = line

					connection_error_reported = False
				except (requests.RequestException, ValueError, json.JSONDecodeError) as exc:
					if not connection_error_reported:
						self.log_signal.emit(f"Wi-Fi log unavailable; retrying: {exc}")
						connection_error_reported = True

				for _ in range(5):
					if self._stop_requested:
						break
					self.msleep(50)
		self._session = None

		if pending:
			self.log_signal.emit(pending)

	def stop(self):
		self._stop_requested = True
		if self._session is not None:
			self._session.close()

class PowerplantManager(QWidget):
	def __init__(self):
		super().__init__()
		
		self.board_types = ["Powerplant", "Substation", "Mainboard"]
		self.device_types = [
			"TYPE_UNKNOWN", "TYPE_NPP", "TYPE_GAS", "TYPE_BATTERY", 
			"TYPE_COAL", "TYPE_WIND", "TYPE_HYDRO", "TYPE_HYDRO_PUMPED", "TYPE_SOLAR"
		]
		
		self.used_uids = self.load_uids()
		self.upload_thread = None
		self.monitor_thread = None
		self.init_ui()

	def load_uids(self):
		if os.path.exists(UID_FILE):
			with open(UID_FILE, 'r') as f:
				try:
					return json.load(f)
				except json.JSONDecodeError:
					return []
		return []

	def save_uid(self, uid_hex, device_type):
		self.used_uids.append({"uid": uid_hex, "type": device_type})
		with open(UID_FILE, 'w') as f:
			json.dump(self.used_uids, f, indent=4)

	def init_ui(self):
		self.setWindowTitle('ENAK Board Flasher')
		layout = QVBoxLayout()

		self.board_combo = QComboBox()
		self.board_combo.addItems(self.board_types)
		self.board_combo.currentTextChanged.connect(self.update_board_ui)
		layout.addWidget(self.board_combo)

		self.type_combo = QComboBox()
		self.type_combo.addItems(self.device_types)
		layout.addWidget(self.type_combo)

		self.mainboard_form_widget = QWidget()
		self.mainboard_form_layout = QFormLayout(self.mainboard_form_widget)

		self.wifi_ssid_input = QLineEdit()
		self.wifi_password_input = QLineEdit()
		self.wifi_password_input.setEchoMode(QLineEdit.Password)
		self.server_endpoint_input = QLineEdit()
		self.board_username_input = QLineEdit()
		self.board_password_input = QLineEdit()
		self.board_password_input.setEchoMode(QLineEdit.Password)
		self.upload_method_combo = QComboBox()
		self.upload_method_combo.addItems(["USB / serial", "Wi-Fi OTA"])
		self.upload_method_combo.currentTextChanged.connect(self.update_upload_method_ui)
		self.ota_host_input = QLineEdit()
		self.ota_port_input = QLineEdit("8080")
		self.ota_password_input = QLineEdit()
		self.ota_password_input.setEchoMode(QLineEdit.Password)
		self.ota_hostname_input = QLineEdit("enak-mainboard")
		self.api_ca_cert_file_input = QLineEdit()
		self.firmware_version_input = QLineEdit("0.1.0")
		self.firmware_channel_input = QLineEdit("stable")
		self.firmware_download_url_input = QLineEdit()
		self.firmware_artifact_dir_input = QLineEdit()
		self.firmware_manifest_file_input = QLineEdit()
		self.firmware_notes_url_input = QLineEdit()
		self.firmware_config_schema_input = QLineEdit("1")

		self.show_passwords_checkbox = QCheckBox("Show passwords")
		self.show_passwords_checkbox.toggled.connect(self.on_show_passwords_toggled)

		self.mainboard_form_layout.addRow("WiFi SSID", self.wifi_ssid_input)
		self.mainboard_form_layout.addRow("WiFi Password", self.wifi_password_input)
		self.mainboard_form_layout.addRow("Server Endpoint", self.server_endpoint_input)
		self.mainboard_form_layout.addRow("Board Username", self.board_username_input)
		self.mainboard_form_layout.addRow("Board Password", self.board_password_input)
		self.mainboard_form_layout.addRow("Upload method", self.upload_method_combo)
		self.mainboard_form_layout.addRow("OTA host / IP", self.ota_host_input)
		self.mainboard_form_layout.addRow("OTA port", self.ota_port_input)
		self.mainboard_form_layout.addRow("OTA password", self.ota_password_input)
		self.mainboard_form_layout.addRow("OTA hostname", self.ota_hostname_input)
		self.mainboard_form_layout.addRow("API CA certificate file", self.api_ca_cert_file_input)
		self.mainboard_form_layout.addRow("Firmware version", self.firmware_version_input)
		self.mainboard_form_layout.addRow("Firmware channel", self.firmware_channel_input)
		self.mainboard_form_layout.addRow("Firmware download URL", self.firmware_download_url_input)
		self.mainboard_form_layout.addRow("Artifact directory", self.firmware_artifact_dir_input)
		self.mainboard_form_layout.addRow("Manifest file", self.firmware_manifest_file_input)
		self.mainboard_form_layout.addRow("Release notes URL", self.firmware_notes_url_input)
		self.mainboard_form_layout.addRow("Config schema", self.firmware_config_schema_input)
		self.mainboard_form_layout.addRow("", self.show_passwords_checkbox)

		layout.addWidget(self.mainboard_form_widget)

		self.port_combo = QComboBox()
		self.port_combo.setEditable(True)
		self.refresh_ports_btn = QPushButton("Refresh Ports")
		self.refresh_ports_btn.clicked.connect(self.refresh_ports)
		layout.addWidget(self.port_combo)
		layout.addWidget(self.refresh_ports_btn)

		self.upload_btn = QPushButton('Generate UID and Upload')
		self.upload_btn.clicked.connect(self.start_upload)
		layout.addWidget(self.upload_btn)

		self.artifact_btn = QPushButton('Build Firmware + Manifest')
		self.artifact_btn.clicked.connect(self.generate_artifacts)
		layout.addWidget(self.artifact_btn)

		self.monitor_checkbox = QCheckBox("Show serial output after upload")
		layout.addWidget(self.monitor_checkbox)

		self.monitor_btn = QPushButton("Show serial")
		self.monitor_btn.clicked.connect(self.toggle_monitor)
		layout.addWidget(self.monitor_btn)

		self.output_text = QTextEdit()
		self.output_text.setReadOnly(True)
		# Monospaced font for build logs
		self.output_text.setStyleSheet("font-family: monospace;") 
		layout.addWidget(self.output_text)

		self.setLayout(layout)
		self.resize(600, 400)
		self.refresh_ports()
		self.update_board_ui()
		self.update_upload_method_ui()

	def update_board_ui(self):
		board_kind = self.board_combo.currentText()
		is_powerplant = board_kind == "Powerplant"
		is_mainboard = board_kind == "Mainboard"
		self.type_combo.setEnabled(is_powerplant)
		self.type_combo.setVisible(is_powerplant)
		self.mainboard_form_widget.setVisible(is_mainboard)
		self.upload_btn.setText({
			"Powerplant": "Generate UID and Upload",
			"Substation": "Upload Substation",
			"Mainboard": "Upload Mainboard",
		}.get(board_kind, "Upload"))
		self.artifact_btn.setVisible(is_mainboard)
		self.artifact_btn.setEnabled(is_mainboard and not (self.upload_thread and self.upload_thread.isRunning()))
		if is_mainboard:
			self.prefill_mainboard_fields()
		self.update_upload_method_ui()

	def on_show_passwords_toggled(self, checked):
		mode = QLineEdit.Normal if checked else QLineEdit.Password
		self.wifi_password_input.setEchoMode(mode)
		self.board_password_input.setEchoMode(mode)
		self.ota_password_input.setEchoMode(mode)

	def update_upload_method_ui(self):
		is_mainboard = self.board_combo.currentText() == "Mainboard"
		if not is_mainboard and self.upload_method_combo.currentText() != "USB / serial":
			self.upload_method_combo.blockSignals(True)
			self.upload_method_combo.setCurrentText("USB / serial")
			self.upload_method_combo.blockSignals(False)
		self.upload_method_combo.setEnabled(is_mainboard)
		is_ota = is_mainboard and self.upload_method_combo.currentText() == "Wi-Fi OTA"

		for widget in (self.ota_host_input, self.ota_port_input):
			widget.setEnabled(is_ota)

		self.ota_password_input.setEnabled(is_ota)

		if hasattr(self, "port_combo"):
			self.port_combo.setEnabled(not is_ota)
			self.refresh_ports_btn.setEnabled(not is_ota)
			self.monitor_checkbox.setText("Show output after upload")
			self.monitor_btn.setText("Stop log" if self.monitor_thread and self.monitor_thread.isRunning()
				else ("Show Wi-Fi log" if is_ota else "Show serial"))

	def stop_monitor(self):
		if self.monitor_thread and self.monitor_thread.isRunning():
			thread = self.monitor_thread
			thread.stop()
			if not thread.wait(5000):
				self.log("ERROR: Log monitor did not stop; upload was not started.")
				return False
		self.monitor_thread = None
		self.update_upload_method_ui()
		return True

	def start_monitor(self, env_name=None):
		if not self.stop_monitor():
			return
		board_kind = self.board_combo.currentText()
		is_mainboard = board_kind == "Mainboard"
		is_ota = is_mainboard and self.upload_method_combo.currentText() == "Wi-Fi OTA"

		if is_ota:
			host = self.ota_host_input.text().strip()
			port_text = self.ota_port_input.text().strip()
			password = self.ota_password_input.text().strip()
			if not host or not port_text or not password:
				self.log("ERROR: OTA host, port, and password are required for Wi-Fi logs.")
				return
			try:
				port = int(port_text)
			except ValueError:
				self.log("ERROR: OTA port must be an integer.")
				return
			self.monitor_thread = WifiLogThread(host, port, password)
		else:
			if env_name is None:
				env_name = {
					"Powerplant": "powerplant",
					"Substation": "substation",
					"Mainboard": "mainboard",
				}.get(board_kind)
			if env_name is None:
				self.log(f"ERROR: Unknown board type: {board_kind}")
				return
			self.monitor_thread = PioMonitorThread(env_name, self.get_selected_port())

		self.monitor_thread.log_signal.connect(self.log)
		self.monitor_thread.finished.connect(self.update_upload_method_ui)
		self.monitor_thread.start()
		self.monitor_btn.setText("Stop log")

	def toggle_monitor(self):
		if self.monitor_thread and self.monitor_thread.isRunning():
			self.stop_monitor()
		else:
			self.start_monitor()

	def log(self, message):
		self.output_text.append(message)
		# Auto-scroll to bottom
		scrollbar = self.output_text.verticalScrollBar()
		scrollbar.setValue(scrollbar.maximum())

	def generate_uid(self):
		existing_uids = [entry["uid"] for entry in self.used_uids]
		while True:
			new_uid = random.randint(0x10000000, 0xFFFFFFFF)
			hex_uid = f"0x{new_uid:08X}"
			if hex_uid not in existing_uids:
				return hex_uid

	def load_mainboard_config(self):
		if not os.path.exists(MAINBOARD_CONFIG_FILE):
			raise FileNotFoundError(f"Missing {MAINBOARD_CONFIG_FILE}")
		parser = configparser.ConfigParser()
		parser.read(MAINBOARD_CONFIG_FILE)
		if "mainboard" not in parser:
			raise ValueError("Missing [mainboard] section in uploader.conf")
		section = parser["mainboard"]
		required_keys = [
			"wifi_ssid",
			"wifi_password",
			"server_endpoint",
			"board_username",
			"board_password",
		]
		missing = [key for key in required_keys if key not in section or not section.get(key, "").strip()]
		if missing:
			raise ValueError(f"Missing keys in [mainboard]: {', '.join(missing)}")
		return {
			"wifi_ssid": section.get("wifi_ssid").strip(),
			"wifi_password": section.get("wifi_password").strip(),
			"server_endpoint": section.get("server_endpoint").strip(),
			"board_username": section.get("board_username").strip(),
			"board_password": section.get("board_password").strip(),
			"ota_host": section.get("ota_host", "").strip(),
			"ota_port": section.get("ota_port", "8080").strip(),
			"ota_password": section.get("ota_password", "").strip(),
			"ota_hostname": section.get("ota_hostname", "enak-mainboard").strip(),
			"api_ca_cert_file": section.get("api_ca_cert_file", "").strip(),
			"firmware_version": section.get("firmware_version", "0.1.0").strip(),
			"firmware_channel": section.get("firmware_channel", "").strip(),
			"firmware_download_url": section.get("firmware_download_url", "").strip(),
			"firmware_artifact_dir": section.get("firmware_artifact_dir", "").strip(),
			"firmware_manifest_file": section.get("firmware_manifest_file", "").strip(),
			"firmware_notes_url": section.get("firmware_notes_url", "").strip(),
			"firmware_config_schema": section.get("firmware_config_schema", "").strip(),
		}

	def prefill_mainboard_fields(self):
		try:
			cfg = self.load_mainboard_config()
		except (FileNotFoundError, ValueError) as exc:
			self.log(f"ERROR: {str(exc)}")
			return
		self.wifi_ssid_input.setText(cfg["wifi_ssid"])
		self.wifi_password_input.setText(cfg["wifi_password"])
		self.server_endpoint_input.setText(cfg["server_endpoint"])
		self.board_username_input.setText(cfg["board_username"])
		self.board_password_input.setText(cfg["board_password"])
		self.ota_host_input.setText(cfg["ota_host"])
		self.ota_port_input.setText(cfg["ota_port"])
		self.ota_password_input.setText(cfg["ota_password"])
		self.ota_hostname_input.setText(cfg["ota_hostname"])
		self.api_ca_cert_file_input.setText(cfg["api_ca_cert_file"])
		self.firmware_version_input.setText(cfg["firmware_version"])
		self.firmware_channel_input.setText(cfg["firmware_channel"])
		self.firmware_download_url_input.setText(cfg["firmware_download_url"])
		self.firmware_artifact_dir_input.setText(cfg["firmware_artifact_dir"])
		self.firmware_manifest_file_input.setText(cfg["firmware_manifest_file"])
		self.firmware_notes_url_input.setText(cfg["firmware_notes_url"])
		self.firmware_config_schema_input.setText(cfg["firmware_config_schema"])

	def get_mainboard_form_values(self):
		cfg = {
			"wifi_ssid": self.wifi_ssid_input.text().strip(),
			"wifi_password": self.wifi_password_input.text().strip(),
			"server_endpoint": self.server_endpoint_input.text().strip(),
			"board_username": self.board_username_input.text().strip(),
			"board_password": self.board_password_input.text().strip(),
			"ota_host": self.ota_host_input.text().strip(),
			"ota_port": self.ota_port_input.text().strip(),
			"ota_password": self.ota_password_input.text().strip(),
			"ota_hostname": self.ota_hostname_input.text().strip(),
			"api_ca_cert_file": self.api_ca_cert_file_input.text().strip(),
			"firmware_version": self.firmware_version_input.text().strip(),
			"firmware_channel": self.firmware_channel_input.text().strip(),
			"firmware_download_url": self.firmware_download_url_input.text().strip(),
			"firmware_artifact_dir": self.firmware_artifact_dir_input.text().strip(),
			"firmware_manifest_file": self.firmware_manifest_file_input.text().strip(),
			"firmware_notes_url": self.firmware_notes_url_input.text().strip(),
			"firmware_config_schema": self.firmware_config_schema_input.text().strip(),
		}
		required = [
			"wifi_ssid", "wifi_password", "server_endpoint", "board_username",
			"board_password", "ota_hostname", "firmware_version",
		]
		if self.upload_method_combo.currentText() == "Wi-Fi OTA":
			required.extend(["ota_host", "ota_port", "ota_password"])
		missing = [key for key in required if not cfg[key]]
		if missing:
			raise ValueError(f"Missing values: {', '.join(missing)}")
		if cfg["ota_password"] and len(cfg["ota_password"]) < 8:
			raise ValueError("OTA password must contain at least 8 characters")
		try:
			cfg["ota_port"] = str(int(cfg["ota_port"] or "8080"))
		except ValueError as exc:
			raise ValueError("OTA port must be an integer") from exc
		return cfg

	def get_artifact_values(self, cfg):
		required = [
			"ota_password", "firmware_channel", "firmware_download_url",
			"firmware_artifact_dir", "firmware_manifest_file", "firmware_config_schema",
		]
		missing = [key for key in required if not cfg[key]]
		if missing:
			raise ValueError(f"Missing values for artifact generation: {', '.join(missing)}")
		if len(cfg["ota_password"]) < 8:
			raise ValueError("OTA password must contain at least 8 characters")
		if cfg["firmware_channel"] not in ("stable", "prerelease"):
			raise ValueError("Firmware channel must be stable or prerelease")
		if not cfg["firmware_download_url"].startswith("https://"):
			raise ValueError("Firmware download URL must use HTTPS")
		read_api_ca_cert(cfg)
		try:
			cfg["firmware_config_schema"] = int(cfg["firmware_config_schema"])
		except ValueError as exc:
			raise ValueError("Config schema must be an integer") from exc
		return cfg

	def refresh_ports(self):
		current_text = self.port_combo.currentText().strip()
		self.port_combo.clear()
		self.port_combo.addItem("Auto (platformio.ini)", "")
		port_filter = self.get_port_filter()
		try:
			output = subprocess.check_output(
				["pio", "device", "list", "--json-output"],
				text=True
			)
			devices = json.loads(output)
			for device in devices:
				port = device.get("port")
				description = device.get("description") or port
				if port:
					if port_filter and not port_filter.search(port):
						continue
					self.port_combo.addItem(f"{port} ({description})", port)
		except Exception as exc:
			self.log(f"ERROR: Failed to list ports: {str(exc)}")
			return

		if current_text:
			index = self.port_combo.findText(current_text)
			if index >= 0:
				self.port_combo.setCurrentIndex(index)
			else:
				self.port_combo.setEditText(current_text)
		elif self.port_combo.count() > 1:
			for idx in range(1, self.port_combo.count()):
				if self.port_combo.itemData(idx):
					self.port_combo.setCurrentIndex(idx)
					break

	def get_selected_port(self):
		data = self.port_combo.currentData()
		if data:
			return data
		text = self.port_combo.currentText().strip()
		if text and text != "Auto (platformio.ini)":
			return text
		return None

	def get_port_filter(self):
		if not os.path.exists(MAINBOARD_CONFIG_FILE):
			return None
		parser = configparser.ConfigParser()
		parser.read(MAINBOARD_CONFIG_FILE)
		regex_value = None
		if parser.has_section("ports"):
			regex_value = parser.get("ports", "filter_regex", fallback=None)
		if not regex_value and parser.has_section("mainboard"):
			regex_value = parser.get("mainboard", "port_filter_regex", fallback=None)
		if not regex_value:
			return None
		regex_value = regex_value.strip()
		if not regex_value:
			return None
		try:
			return re.compile(regex_value)
		except re.error as exc:
			self.log(f"ERROR: Invalid port filter regex: {str(exc)}")
			return None

	def _escape_define_value(self, value):
		return value.replace("\\", "\\\\").replace("\"", "\\\"")

	def write_api_ca_cert_header(self, cfg):
		api_ca_cert = read_api_ca_cert(cfg)
		c_literal = (
			api_ca_cert.replace("\\", "\\\\")
			.replace("\"", "\\\"")
			.replace("\r", "")
			.replace("\n", "\\n")
		)
		GENERATED_API_CA_CERT_HEADER.write_text(
			"#pragma once\n"
			f'#define API_CA_CERT "{c_literal}"\n',
			encoding="utf-8",
		)

	def build_mainboard_flags(self, cfg):
		self.write_api_ca_cert_header(cfg)
		return " ".join([
			f'-DWIFI_SSID=\\"{self._escape_define_value(cfg["wifi_ssid"])}\\"',
			f'-DWIFI_PASS=\\"{self._escape_define_value(cfg["wifi_password"])}\\"',
			f'-DAPI_BASE_URL=\\"{self._escape_define_value(cfg["server_endpoint"])}\\"',
			f'-DBOARD_USERNAME=\\"{self._escape_define_value(cfg["board_username"])}\\"',
			f'-DBOARD_PASSWORD=\\"{self._escape_define_value(cfg["board_password"])}\\"',
			f'-DOTA_PASSWORD=\\"{self._escape_define_value(cfg["ota_password"] or "CHANGE_ME")}\\"',
			f'-DOTA_HOSTNAME=\\"{self._escape_define_value(cfg["ota_hostname"])}\\"',
			f'-DOTA_PORT={cfg["ota_port"] or "8080"}',
			'-include src/GeneratedApiCaCert.h',
			f'-DFIRMWARE_VERSION=\\"{self._escape_define_value(cfg["firmware_version"])}\\"',
		])

	def generate_artifacts(self):
		if self.board_combo.currentText() != "Mainboard":
			self.log("ERROR: Firmware artifacts can only be generated for Mainboard.")
			return
		try:
			cfg = self.get_artifact_values(self.get_mainboard_form_values())
			artifact_dir = resolve_project_path(cfg["firmware_artifact_dir"])
			manifest_path = resolve_project_path(cfg["firmware_manifest_file"])
		except (ValueError, OSError) as exc:
			self.log(f"ERROR: {exc}")
			return

		self.upload_btn.setEnabled(False)
		self.artifact_btn.setEnabled(False)
		self.output_text.clear()
		if self.monitor_thread and self.monitor_thread.isRunning():
			self.log("--- Stopping active log monitor for artifact build ---")
			self.stop_monitor()

		self.upload_thread = PioArtifactThread(
			self.build_mainboard_flags(cfg),
			cfg["firmware_version"],
			cfg["firmware_channel"],
			cfg["firmware_download_url"],
			artifact_dir,
			manifest_path,
			cfg["firmware_notes_url"],
			cfg["firmware_config_schema"],
		)
		self.upload_thread.log_signal.connect(self.log)
		self.upload_thread.finished_signal.connect(self.on_artifact_finished)
		self.upload_thread.start()

	def on_artifact_finished(self, success):
		self.upload_btn.setEnabled(True)
		self.artifact_btn.setEnabled(self.board_combo.currentText() == "Mainboard")
		self.log("\\nSUCCESS: Firmware artifact generation complete." if success
			else "\\nFAILED: Firmware artifact generation failed.")

	def start_upload(self):
		board_kind = self.board_combo.currentText()
		self.upload_btn.setEnabled(False)
		self.artifact_btn.setEnabled(False)
		self.output_text.clear()
		selected_port = self.get_selected_port()

		if self.monitor_thread and self.monitor_thread.isRunning():
			self.log("--- Stopping active log monitor for upload ---")
			if not self.stop_monitor():
				self.upload_btn.setEnabled(True)
				self.artifact_btn.setEnabled(self.board_combo.currentText() == "Mainboard")
				return

		if board_kind == "Powerplant":
			selected_type = self.type_combo.currentText()
			hex_uid = self.generate_uid()
			build_flags = f"-DDEVICE_TYPE={selected_type} -DDEVICE_UID={hex_uid}"
			self.upload_thread = PioUploadThread("powerplant", build_flags, selected_port)
			self.upload_thread.log_signal.connect(self.log)
			self.upload_thread.finished_signal.connect(
				lambda success: self.on_upload_finished(success, "powerplant", board_kind, hex_uid, selected_type)
			)
			self.upload_thread.start()
			return

		if board_kind == "Substation":
			self.upload_thread = PioUploadThread("substation", None, selected_port)
			self.upload_thread.log_signal.connect(self.log)
			self.upload_thread.finished_signal.connect(
				lambda success: self.on_upload_finished(success, "substation", board_kind)
			)
			self.upload_thread.start()
			return

		try:
			cfg = self.get_mainboard_form_values()
			build_flags = self.build_mainboard_flags(cfg)
		except (ValueError, OSError) as exc:
			self.log(f"ERROR: {str(exc)}")
			self.upload_btn.setEnabled(True)
			return

		is_ota = self.upload_method_combo.currentText() == "Wi-Fi OTA"
		if is_ota:
			self.upload_thread = PioOtaThread(
				"mainboard",
				build_flags,
				cfg["ota_host"],
				int(cfg["ota_port"]),
				cfg["ota_password"],
			)
		else:
			self.upload_thread = PioUploadThread("mainboard", build_flags, selected_port)
		self.upload_thread.log_signal.connect(self.log)
		self.upload_thread.finished_signal.connect(
			lambda success: self.on_upload_finished(
				success, "mainboard", board_kind
			)
		)
		self.upload_thread.start()

	def on_upload_finished(self, success, env_name, board_kind, hex_uid=None, selected_type=None):
		self.upload_btn.setEnabled(True)
		self.artifact_btn.setEnabled(self.board_combo.currentText() == "Mainboard")
		if success:
			if board_kind == "Powerplant" and hex_uid and selected_type:
				self.save_uid(hex_uid, selected_type)
				self.log(f"\nSUCCESS: Upload complete. {hex_uid} assigned to {selected_type} and saved.")
			elif board_kind == "Substation":
				self.log("\nSUCCESS: Substation upload complete.")
			else:
				self.log("\nSUCCESS: Mainboard upload complete.")

			if self.monitor_checkbox.isChecked():
				self.start_monitor(env_name)
		else:
			if board_kind == "Powerplant":
				self.log("\nFAILED: Upload aborted. UID not saved.")
			elif board_kind == "Substation":
				self.log("\nFAILED: Substation upload aborted.")
			else:
				self.log("\nFAILED: Mainboard upload aborted.")

if __name__ == '__main__':
	app = QApplication(sys.argv)
	window = PowerplantManager()
	window.show()
	sys.exit(app.exec_())
