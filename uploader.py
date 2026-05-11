import sys
import os
import random
import json
import subprocess
from PyQt5.QtWidgets import QApplication, QWidget, QVBoxLayout, QComboBox, QPushButton, QTextEdit
from PyQt5.QtCore import QThread, pyqtSignal

UID_FILE = "used_uids.json"

class PioUploadThread(QThread):
	log_signal = pyqtSignal(str)
	finished_signal = pyqtSignal(bool)

	def __init__(self, device_type, uid_hex):
		super().__init__()
		self.device_type = device_type
		self.uid_hex = uid_hex

	def run(self):
		env = os.environ.copy()
		
		build_flags = f"-DDEVICE_TYPE={self.device_type} -DDEVICE_UID={self.uid_hex}"
		env["PLATFORMIO_BUILD_FLAGS"] = build_flags

		self.log_signal.emit(f"--- Starting build with flags: {build_flags} ---")

		try:
			process = subprocess.Popen(
				["pio", "run", "-e", "powerplant", "-t", "upload"],
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

class PowerplantManager(QWidget):
	def __init__(self):
		super().__init__()
		
		self.device_types = [
			"TYPE_UNKNOWN", "TYPE_NPP", "TYPE_GAS", "TYPE_BATTERY", 
			"TYPE_COAL", "TYPE_WIND", "TYPE_HYDRO", "TYPE_HYDRO_PUMPED", "TYPE_SOLAR"
		]
		
		self.used_uids = self.load_uids()
		self.upload_thread = None
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
		self.setWindowTitle('Powerplant Flasher')
		layout = QVBoxLayout()

		self.type_combo = QComboBox()
		self.type_combo.addItems(self.device_types)
		layout.addWidget(self.type_combo)

		self.upload_btn = QPushButton('Generate UID and Upload')
		self.upload_btn.clicked.connect(self.start_upload)
		layout.addWidget(self.upload_btn)

		self.output_text = QTextEdit()
		self.output_text.setReadOnly(True)
		# Monospaced font for build logs
		self.output_text.setStyleSheet("font-family: monospace;") 
		layout.addWidget(self.output_text)

		self.setLayout(layout)
		self.resize(600, 400)

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

	def start_upload(self):
		selected_type = self.type_combo.currentText()
		hex_uid = self.generate_uid()

		self.upload_btn.setEnabled(False)
		self.output_text.clear()
		
		self.upload_thread = PioUploadThread(selected_type, hex_uid)
		self.upload_thread.log_signal.connect(self.log)
		self.upload_thread.finished_signal.connect(lambda success: self.on_upload_finished(success, hex_uid, selected_type))
		self.upload_thread.start()

	def on_upload_finished(self, success, hex_uid, selected_type):
		self.upload_btn.setEnabled(True)
		if success:
			self.save_uid(hex_uid, selected_type)
			self.log(f"\nSUCCESS: Upload complete. {hex_uid} assigned to {selected_type} and saved.")
		else:
			self.log("\nFAILED: Upload aborted. UID not saved.")

if __name__ == '__main__':
	app = QApplication(sys.argv)
	window = PowerplantManager()
	window.show()
	sys.exit(app.exec_())