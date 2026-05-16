import sys
import os
import random
import json
import subprocess
import configparser
import re
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

UID_FILE = "used_uids.json"
MAINBOARD_CONFIG_FILE = "uploader.conf"

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
		
		env["PLATFORMIO_BUILD_FLAGS"] = self.build_flags

		self.log_signal.emit(f"--- Starting build with flags: {self.build_flags} ---")

		try:
			command = ["pio", "run", "-e", self.env_name, "-t", "upload"]
			if self.port:
				command.extend(["--upload-port", self.port])
			process = subprocess.Popen(
				command,
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
			command = ["pio", "device", "monitor", "-e", self.env_name]
			if self.port:
				command.extend(["--port", self.port])
			self.process = subprocess.Popen(
				command,
				stdout=subprocess.PIPE,
				stderr=subprocess.STDOUT,
				text=True
			)

			for line in self.process.stdout:
				if self._stop_requested:
					break
				self.log_signal.emit(line.rstrip())

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

class PowerplantManager(QWidget):
	def __init__(self):
		super().__init__()
		
		self.board_types = ["Powerplant", "Mainboard"]
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

		self.show_passwords_checkbox = QCheckBox("Show passwords")
		self.show_passwords_checkbox.toggled.connect(self.on_show_passwords_toggled)

		self.mainboard_form_layout.addRow("WiFi SSID", self.wifi_ssid_input)
		self.mainboard_form_layout.addRow("WiFi Password", self.wifi_password_input)
		self.mainboard_form_layout.addRow("Server Endpoint", self.server_endpoint_input)
		self.mainboard_form_layout.addRow("Board Username", self.board_username_input)
		self.mainboard_form_layout.addRow("Board Password", self.board_password_input)
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

		self.monitor_checkbox = QCheckBox("Show serial output after upload")
		layout.addWidget(self.monitor_checkbox)

		self.output_text = QTextEdit()
		self.output_text.setReadOnly(True)
		# Monospaced font for build logs
		self.output_text.setStyleSheet("font-family: monospace;") 
		layout.addWidget(self.output_text)

		self.setLayout(layout)
		self.resize(600, 400)
		self.refresh_ports()
		self.update_board_ui()

	def update_board_ui(self):
		is_powerplant = self.board_combo.currentText() == "Powerplant"
		self.type_combo.setEnabled(is_powerplant)
		self.type_combo.setVisible(is_powerplant)
		self.mainboard_form_widget.setVisible(not is_powerplant)
		self.upload_btn.setText("Generate UID and Upload" if is_powerplant else "Upload Mainboard")
		if not is_powerplant:
			self.prefill_mainboard_fields()

	def on_show_passwords_toggled(self, checked):
		mode = QLineEdit.Normal if checked else QLineEdit.Password
		self.wifi_password_input.setEchoMode(mode)
		self.board_password_input.setEchoMode(mode)

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

	def get_mainboard_form_values(self):
		cfg = {
			"wifi_ssid": self.wifi_ssid_input.text().strip(),
			"wifi_password": self.wifi_password_input.text().strip(),
			"server_endpoint": self.server_endpoint_input.text().strip(),
			"board_username": self.board_username_input.text().strip(),
			"board_password": self.board_password_input.text().strip(),
		}
		missing = [key for key, value in cfg.items() if not value]
		if missing:
			raise ValueError(f"Missing values: {', '.join(missing)}")
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

	def build_mainboard_flags(self, cfg):
		return " ".join([
			f'-DWIFI_SSID=\\"{self._escape_define_value(cfg["wifi_ssid"])}\\"',
			f'-DWIFI_PASS=\\"{self._escape_define_value(cfg["wifi_password"])}\\"',
			f'-DAPI_BASE_URL=\\"{self._escape_define_value(cfg["server_endpoint"])}\\"',
			f'-DBOARD_USERNAME=\\"{self._escape_define_value(cfg["board_username"])}\\"',
			f'-DBOARD_PASSWORD=\\"{self._escape_define_value(cfg["board_password"])}\\"',
		])

	def start_upload(self):
		board_kind = self.board_combo.currentText()
		self.upload_btn.setEnabled(False)
		self.output_text.clear()
		selected_port = self.get_selected_port()

		if self.monitor_thread and self.monitor_thread.isRunning():
			self.log("--- Stopping serial monitor to free the port ---")
			self.monitor_thread.stop()
			self.monitor_thread.wait(3000)

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

		try:
			cfg = self.get_mainboard_form_values()
			build_flags = self.build_mainboard_flags(cfg)
		except ValueError as exc:
			self.log(f"ERROR: {str(exc)}")
			self.upload_btn.setEnabled(True)
			return

		self.upload_thread = PioUploadThread("mainboard", build_flags, selected_port)
		self.upload_thread.log_signal.connect(self.log)
		self.upload_thread.finished_signal.connect(
			lambda success: self.on_upload_finished(success, "mainboard", board_kind)
		)
		self.upload_thread.start()

	def on_upload_finished(self, success, env_name, board_kind, hex_uid=None, selected_type=None):
		self.upload_btn.setEnabled(True)
		if success:
			if board_kind == "Powerplant" and hex_uid and selected_type:
				self.save_uid(hex_uid, selected_type)
				self.log(f"\nSUCCESS: Upload complete. {hex_uid} assigned to {selected_type} and saved.")
			else:
				self.log("\nSUCCESS: Mainboard upload complete.")

			if self.monitor_checkbox.isChecked():
				if self.monitor_thread and self.monitor_thread.isRunning():
					self.monitor_thread.stop()
				self.monitor_thread = PioMonitorThread(env_name, self.get_selected_port())
				self.monitor_thread.log_signal.connect(self.log)
				self.monitor_thread.start()
		else:
			if board_kind == "Powerplant":
				self.log("\nFAILED: Upload aborted. UID not saved.")
			else:
				self.log("\nFAILED: Mainboard upload aborted.")

if __name__ == '__main__':
	app = QApplication(sys.argv)
	window = PowerplantManager()
	window.show()
	sys.exit(app.exec_())