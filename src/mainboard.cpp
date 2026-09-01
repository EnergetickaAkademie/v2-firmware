#include <Arduino.h>
#include <vector>
#include "Config.h"
#include "RuntimeConfig.h"
#include "GameState.h"
#include "NetworkManager.h"
#include "OtaManager.h"
#include "StatusLedManager.h"
#include "SubstationManager.h"
#include "PeripheralFactory.h"
#include "BuildingTypes.h"
#include <algorithm>
#include <PN532_I2C.h>
#include <PN532.h>
#include <WiFi.h>
#include <esp_system.h>
#include "DebugLog.h"

#define Serial DebugLog

PeripheralFactory factory;
ShiftRegisterChain* outChain = nullptr;
InputShiftRegisterChain* inChain = nullptr;

std::vector<ShiftButton*> buttons;
std::vector<ShiftEncoder*> encoders;
std::vector<Bargraph*> bargraphs;

SegmentDisplayPair disp1, disp2, disp3;
SegmentDisplayPair::Half coalDisplay, hydroDisplay, gasDisplay, nuclearDisplay, batteryDisplay, windPvDisplay;
SegmentDisplay* consumptionDisp = nullptr;
SegmentDisplay* productionDisp = nullptr;

uint32_t lastLedToggleMs = 0;
uint32_t lastCountsUpdateMs = 0;
uint32_t lastDisplaysUpdateMs = 0;
uint32_t lastBargraphsUpdateMs = 0;
bool ledState = false;

PN532_I2C pn532_i2c(Wire);
PN532 nfc(pn532_i2c);

// Keep peripheral updates in normal loop context. PeripheralFactory::update()
// traverses C++ containers and calls GPIO/shift-register code, so it must not
// run from a hardware timer ISR.
uint32_t lastPeripheralUpdateUs = 0;

const char* resetReasonName(esp_reset_reason_t reason) {
	switch (reason) {
		case ESP_RST_POWERON: return "power-on";
		case ESP_RST_EXT: return "external";
		case ESP_RST_SW: return "software";
		case ESP_RST_PANIC: return "panic";
		case ESP_RST_INT_WDT: return "interrupt-watchdog";
		case ESP_RST_TASK_WDT: return "task-watchdog";
		case ESP_RST_WDT: return "watchdog";
		case ESP_RST_BROWNOUT: return "brownout";
		case ESP_RST_SDIO: return "SDIO";
		default: return "unknown";
	}
}

namespace {
constexpr size_t NTAG213_USER_BYTES = 144;
constexpr size_t INITIAL_NDEF_READ_BYTES = 32;
constexpr uint32_t RESET_HOLD_MS = 2000;
constexpr uint8_t NFC_PROTOCOL_VERSION = 2;
constexpr uint8_t ADMIN_COMMAND_RESET_BUILDINGS = 1;
constexpr uint8_t NDEF_TNF_EXTERNAL_TYPE = 0x04;

struct NdefRecordView {
	uint8_t tnf = 0;
	const uint8_t* type = nullptr;
	size_t typeLength = 0;
	const uint8_t* payload = nullptr;
	size_t payloadLength = 0;
};

uint32_t crc32(const uint8_t* data, size_t length) {
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < length; ++i) {
		crc ^= data[i];
		for (uint8_t bit = 0; bit < 8; ++bit) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return crc ^ 0xFFFFFFFFu;
}

bool findNdefMessage(const uint8_t* data, size_t available,
					 size_t& messageOffset, size_t& messageLength) {
	size_t offset = 0;
	while (offset < available) {
		const uint8_t tag = data[offset++];
		if (tag == 0x00) continue;
		if (tag == 0xFE) return false;

		if (offset >= available) return false;
		size_t valueLength = 0;
		if (data[offset] == 0xFF) {
			if (offset + 2 >= available) return false;
			valueLength = (static_cast<size_t>(data[offset + 1]) << 8) |
						  data[offset + 2];
			offset += 3;
		} else {
			valueLength = data[offset++];
		}

		if (tag == 0x03) {
			messageOffset = offset;
			messageLength = valueLength;
			return messageOffset + messageLength <= NTAG213_USER_BYTES;
		}

		if (offset + valueLength > available) return false;
		offset += valueLength;
	}
	return false;
}

bool parseFirstNdefRecord(const uint8_t* data, size_t dataLength,
						  NdefRecordView& record) {
	size_t messageOffset = 0;
	size_t messageLength = 0;
	if (!findNdefMessage(data, dataLength, messageOffset, messageLength) ||
		messageOffset + messageLength > dataLength || messageLength < 3) {
		return false;
	}

	const size_t messageEnd = messageOffset + messageLength;
	size_t offset = messageOffset;
	const uint8_t header = data[offset++];
	if ((header & 0xC0) != 0xC0) return false;  // Require one complete record.
	if ((header & 0x20) != 0) return false;  // Chunked records are unsupported.
	const bool shortRecord = (header & 0x10) != 0;
	const bool hasId = (header & 0x08) != 0;
	const uint8_t typeLength = data[offset++];

	uint32_t payloadLength = 0;
	if (shortRecord) {
		if (offset >= messageEnd) return false;
		payloadLength = data[offset++];
	} else {
		if (offset + 4 > messageEnd) return false;
		payloadLength =
			(static_cast<uint32_t>(data[offset]) << 24) |
			(static_cast<uint32_t>(data[offset + 1]) << 16) |
			(static_cast<uint32_t>(data[offset + 2]) << 8) |
			static_cast<uint32_t>(data[offset + 3]);
		offset += 4;
	}

	uint8_t idLength = 0;
	if (hasId) {
		if (offset >= messageEnd) return false;
		idLength = data[offset++];
	}
	if (offset + typeLength + idLength + payloadLength != messageEnd) return false;

	record.tnf = header & 0x07;
	record.type = data + offset;
	record.typeLength = typeLength;
	offset += typeLength + idLength;
	record.payload = data + offset;
	record.payloadLength = payloadLength;
	return true;
}

bool externalRecordTypeEquals(const NdefRecordView& record, const char* expected) {
	const size_t length = strlen(expected);
	return record.tnf == NDEF_TNF_EXTERNAL_TYPE &&
		record.typeLength == length &&
		memcmp(record.type, expected, length) == 0;
}

bool readUltralightNdef(uint8_t* data, size_t& dataLength) {
	memset(data, 0, NTAG213_USER_BYTES);
	for (uint8_t page = 4; page < 12; ++page) {
		if (!nfc.mifareultralight_ReadPage(
				page, data + static_cast<size_t>(page - 4) * 4)) {
			return false;
		}
	}
	dataLength = INITIAL_NDEF_READ_BYTES;

	size_t messageOffset = 0;
	size_t messageLength = 0;
	if (!findNdefMessage(data, dataLength, messageOffset, messageLength)) {
		return true;
	}
	const size_t required = messageOffset + messageLength;
	if (required <= dataLength) {
		dataLength = required;
		return true;
	}
	if (required > NTAG213_USER_BYTES) return false;

	const size_t requiredPages = (required + 3) / 4;
	for (size_t pageIndex = INITIAL_NDEF_READ_BYTES / 4;
		 pageIndex < requiredPages; ++pageIndex) {
		const uint8_t page = static_cast<uint8_t>(4 + pageIndex);
		if (!nfc.mifareultralight_ReadPage(page, data + pageIndex * 4)) {
			return false;
		}
	}
	dataLength = required;
	return true;
}

bool parseWifiProvisioning(const NdefRecordView& record,
						   String& ssid, String& password, uint8_t& security) {
	if (record.payloadLength < 9 || record.payload[0] != NFC_PROTOCOL_VERSION) {
		return false;
	}

	security = record.payload[1];
	const size_t ssidLength = record.payload[2];
	size_t offset = 3;
	if (ssidLength == 0 || ssidLength > 32 ||
		offset + ssidLength + 1 + 4 > record.payloadLength) {
		return false;
	}

	ssid = "";
	ssid.reserve(ssidLength);
	for (size_t i = 0; i < ssidLength; ++i) {
		ssid += static_cast<char>(record.payload[offset + i]);
	}
	offset += ssidLength;

	const size_t passwordLength = record.payload[offset++];
	if (offset + passwordLength + 4 != record.payloadLength) return false;
	password = "";
	password.reserve(passwordLength);
	for (size_t i = 0; i < passwordLength; ++i) {
		password += static_cast<char>(record.payload[offset + i]);
	}
	offset += passwordLength;

	const uint32_t expectedCrc =
		(static_cast<uint32_t>(record.payload[offset]) << 24) |
		(static_cast<uint32_t>(record.payload[offset + 1]) << 16) |
		(static_cast<uint32_t>(record.payload[offset + 2]) << 8) |
		static_cast<uint32_t>(record.payload[offset + 3]);
	return crc32(record.payload, offset) == expectedCrc &&
		security <= 1 &&
		((security == 0 && passwordLength == 0) ||
		 (security == 1 && passwordLength >= 8 && passwordLength <= 63));
}

void playResetConfirmedTone() {
	const uint16_t frequencies[] = {1200, 1700, 2200};
	for (uint16_t frequency : frequencies) {
		tone(BUZZER_PIN, frequency, 90);
		vTaskDelay(pdMS_TO_TICKS(120));
	}
}
} // namespace

void updateDisplays() {
	int32_t combinedBatteryPump = productionByTypeMW[8] + productionByTypeMW[6];
	int32_t combinedWindSolar = productionByTypeMW[2] + productionByTypeMW[1];

	if (currentCoefficient[7] > 0.0) coalDisplay.displayNumber(productionByTypeMW[7], 0);
	else coalDisplay.clear();
	if (currentCoefficient[5] > 0.0) hydroDisplay.displayNumber(productionByTypeMW[5], 0);
	else hydroDisplay.clear();
	if (currentCoefficient[8] > 0.0 || currentCoefficient[6] > 0.0) batteryDisplay.displayNumber(combinedBatteryPump, 0);
	else batteryDisplay.clear();
	if (currentCoefficient[3] > 0.0) nuclearDisplay.displayNumber(productionByTypeMW[3], 0);
	else nuclearDisplay.clear();
	if (currentCoefficient[4] > 0.0) gasDisplay.displayNumber(productionByTypeMW[4], 0);
	else gasDisplay.clear();
	if (currentCoefficient[2] > 0.0 || currentCoefficient[1] > 0.0) windPvDisplay.displayNumber(combinedWindSolar, 0);
	else windPvDisplay.clear();

	if (consumptionDisp) consumptionDisp->displayNumber(currentTotalConsumption_MW, 0);
	if (productionDisp) productionDisp->displayNumber(currentTotalProduction_MW, 0);
}

bool updateBargraphs() {
	bool anyChanged = false;

	for (size_t i = 0; i < DEVICE_COUNT; ++i) {
		if (!encoders[i] || !bargraphs[i]) continue;

		int32_t activeMin = 0;
		int32_t activeMax = 0;
		float displayCoeff = 0.0;

		switch(i) {
			case 0:
				activeMin = (int32_t)(baseMinMW[7] * connectedCount[7] * currentCoefficient[7]);
				activeMax = (int32_t)(baseMaxMW[7] * connectedCount[7] * currentCoefficient[7]);
				displayCoeff = currentCoefficient[7];
				break;
			case 1:
				activeMin = (int32_t)(baseMinMW[5] * connectedCount[5] * currentCoefficient[5]);
				activeMax = (int32_t)(baseMaxMW[5] * connectedCount[5] * currentCoefficient[5]);
				displayCoeff = currentCoefficient[5];
				break;
			case 2:
				activeMin = (int32_t)(baseMinMW[8] * connectedCount[8] * currentCoefficient[8]) +
							(int32_t)(baseMinMW[6] * connectedCount[6] * currentCoefficient[6]);
				activeMax = (int32_t)(baseMaxMW[8] * connectedCount[8] * currentCoefficient[8]) +
							(int32_t)(baseMaxMW[6] * connectedCount[6] * currentCoefficient[6]);
				displayCoeff = max(currentCoefficient[8], currentCoefficient[6]);
				break;
			case 3:
				activeMin = (int32_t)(baseMinMW[3] * connectedCount[3] * currentCoefficient[3]);
				activeMax = (int32_t)(baseMaxMW[3] * connectedCount[3] * currentCoefficient[3]);
				displayCoeff = currentCoefficient[3];
				break;
			case 4:
				activeMin = (int32_t)(baseMinMW[4] * connectedCount[4] * currentCoefficient[4]);
				activeMax = (int32_t)(baseMaxMW[4] * connectedCount[4] * currentCoefficient[4]);
				displayCoeff = currentCoefficient[4];
				break;
			case 5:
				activeMin = (int32_t)(baseMinMW[2] * connectedCount[2] * currentCoefficient[2]) +
							(int32_t)(baseMinMW[1] * connectedCount[1] * currentCoefficient[1]);
				activeMax = (int32_t)(baseMaxMW[2] * connectedCount[2] * currentCoefficient[2]) +
							(int32_t)(baseMaxMW[1] * connectedCount[1] * currentCoefficient[1]);
				displayCoeff = max(currentCoefficient[2], currentCoefficient[1]);
				break;
		}

		int32_t val = encoders[i]->get_value();
		float pct = 0.0f;

		if (displayCoeff <= 0.0 || activeMax == 0) {
			val = 0;
			if(encoders[i]->get_value() != 0) {
				encoders[i]->set_value(0);
				anyChanged = true;
			}
			bargraphs[i]->setEnabled(false);
		} else {
			bargraphs[i]->setEnabled(true);

			if (val < activeMin) {
				val = activeMin;
				encoders[i]->set_value(val);
				anyChanged = true;
			} else if (val > activeMax) {
				val = activeMax;
				encoders[i]->set_value(val);
				anyChanged = true;
			}

			if (activeMax > activeMin) {
				pct = (float)(val - activeMin) / (float)(activeMax - activeMin);
			} else {
				pct = 1.0f;
			}

			bargraphs[i]->setRange(activeMin, activeMax);
			bargraphs[i]->setValue(val);
		}

		encoderPercentages[i] = pct;

		if (encoderValuesMW[i] != val) {
			encoderValuesMW[i] = val;
			anyChanged = true;
		}
	}
	return anyChanged;
}

void nfcTaskImpl(void *pvParameters) {
	String presentedUid;
	String resetArmedUid;
	uint32_t resetArmedAtMs = 0;
	bool resetExecuted = false;
	uint8_t consecutiveNoTagPolls = 0;
	uint32_t noTagPolls = 0;
	uint32_t lastHealthLogMs = millis();
	uint32_t lastErrorLogMs = 0;
	int16_t lastLoggedError = 0;

	Serial.println("[NFC] Passive tag polling started.");

	for (;;) {
		// Flash writes temporarily suspend caches and need predictable access to
		// the Wi-Fi stack. Keep the NFC/I2C task idle for the entire OTA session.
		const bool otaActive = isOtaInProgress();
		setOtaNfcTaskPaused(otaActive);
		if (otaActive) {
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

		if (consumeBuildingResetAcknowledged()) {
			Serial.println("[NFC] Server confirmed the building reset.");
			tone(BUZZER_PIN, 2400, 350);
		}

		uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
		uint8_t uidLength = 0;

		if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 500)) {
			consecutiveNoTagPolls = 0;
			Serial.println("[NFC] Tag detected!");

			String uidStr = "";
			for (uint8_t i = 0; i < uidLength; i++) {
				uidStr += String(uid[i], HEX);
			}

			if (uidStr == presentedUid) {
				if (!resetExecuted && resetArmedUid == uidStr &&
					millis() - resetArmedAtMs >= RESET_HOLD_MS) {
					requestBuildingReset();
					resetExecuted = true;
					statusLedNotifyNfcEvent(StatusNfcEvent::Accepted);
					Serial.println("[NFC] Reset card hold confirmed; building state cleared.");
					playResetConfirmedTone();
				}
				vTaskDelay(pdMS_TO_TICKS(100));
				continue;
			}
			resetArmedUid = "";
			resetArmedAtMs = 0;
			resetExecuted = false;
			presentedUid = uidStr;

			uint8_t data[NTAG213_USER_BYTES] = {0};
			size_t dataLength = 0;
			bool readSuccess = false;

			if (uidLength == 7) {
				readSuccess = readUltralightNdef(data, dataLength);
			} else if (uidLength == 4) {
				uint8_t keyNDEF[6]      = { 0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7 };
				uint8_t keyUniversal[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

				if (nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 4, 0, keyNDEF)) {
					if (nfc.mifareclassic_ReadDataBlock(4, data)) {
						readSuccess = true;
						dataLength = 16;
					}
				}
				else if (nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 4, 0, keyUniversal)) {
					if (nfc.mifareclassic_ReadDataBlock(4, data)) {
						readSuccess = true;
						dataLength = 16;
					}
				}
			}

			if (readSuccess) {
				NdefRecordView record;
				const bool ndefFound = parseFirstNdefRecord(data, dataLength, record);

				if (ndefFound && externalRecordTypeEquals(record, "cz.enak:cmd")) {
					if (record.payloadLength == 2 &&
						record.payload[0] == NFC_PROTOCOL_VERSION &&
						record.payload[1] == ADMIN_COMMAND_RESET_BUILDINGS) {
						resetArmedUid = uidStr;
						resetArmedAtMs = millis();
						resetExecuted = false;
						Serial.println("[NFC] Reset card recognized; hold it in place for two seconds.");
						tone(BUZZER_PIN, 800, 100);
					} else {
						Serial.println("[NFC] Unsupported administrative command record.");
						statusLedNotifyNfcEvent(StatusNfcEvent::Rejected);
						tone(BUZZER_PIN, 300, 300);
					}
					continue;
				}

				if (ndefFound && externalRecordTypeEquals(record, "cz.enak:wifi")) {
					String ssid;
					String password;
					uint8_t security = 0;
					if (parseWifiProvisioning(record, ssid, password, security) &&
						queueWifiProvisioning(ssid, password, security)) {
						Serial.printf("[NFC] WiFi provisioning accepted for SSID: %s\n", ssid.c_str());
						statusLedNotifyNfcEvent(StatusNfcEvent::Accepted);
						tone(BUZZER_PIN, 1500, 100);
						vTaskDelay(pdMS_TO_TICKS(130));
						tone(BUZZER_PIN, 2100, 180);
					} else {
						Serial.println("[NFC] Invalid WiFi provisioning record.");
						statusLedNotifyNfcEvent(StatusNfcEvent::Rejected);
						tone(BUZZER_PIN, 300, 300);
					}
					continue;
				}

				const bool validBuildingRecord =
					ndefFound &&
					externalRecordTypeEquals(record, "cz.enak:building") &&
					record.payloadLength == 2 &&
					record.payload[0] == NFC_PROTOCOL_VERSION &&
					record.payload[1] < BUILDING_COUNT;
				if (validBuildingRecord) {
					const uint8_t buildingType = record.payload[1];
					BuildingScanQueueResult queueResult =
						queueBuildingScan(uidStr, buildingType);

					if (queueResult == BuildingScanQueueResult::Duplicate) {
						Serial.println("[NFC] Already scanned in this scenario, ignoring.");
						vTaskDelay(pdMS_TO_TICKS(150));
						continue;
					}
					if (queueResult == BuildingScanQueueResult::Inactive) {
						Serial.println("[NFC] Scenario is inactive, ignoring tag.");
						vTaskDelay(pdMS_TO_TICKS(150));
						continue;
					}
					if (queueResult == BuildingScanQueueResult::ResetPending) {
						Serial.println("[NFC] Building reset is still synchronizing; scan again after confirmation.");
						statusLedNotifyNfcEvent(StatusNfcEvent::Rejected);
						tone(BUZZER_PIN, 500, 180);
						vTaskDelay(pdMS_TO_TICKS(150));
						continue;
					}
					if (queueResult == BuildingScanQueueResult::Unavailable) {
						Serial.println("[NFC] Scan queue is unavailable, ignoring tag.");
						vTaskDelay(pdMS_TO_TICKS(150));
						continue;
					}

					Serial.println("[NFC] Queued building for server.");
					Serial.printf("[NFC] SUCCESS! UID: %s | Type: 0x%02X | Queued for server.\n",
								  uidStr.c_str(), buildingType);
					statusLedNotifyNfcEvent(StatusNfcEvent::Accepted);
					tone(BUZZER_PIN, 2000, 150);
				} else {
					Serial.println("[NFC] REJECTED. Valid NDEF format not found in memory.");
					statusLedNotifyNfcEvent(StatusNfcEvent::Rejected);
					tone(BUZZER_PIN, 300, 300);
				}
			} else {
				Serial.printf("[NFC] REJECTED. Failed to read memory. UID length: %d\n", uidLength);
				statusLedNotifyNfcEvent(StatusNfcEvent::Rejected);
				tone(BUZZER_PIN, 100, 500);
			}
		} else {
			++noTagPolls;
			const int16_t transportError = pn532_i2c.getLastError();
			const uint32_t now = millis();
			if (transportError < 0 &&
					(transportError != lastLoggedError || now - lastErrorLogMs >= 5000)) {
				Serial.printf("[NFC] I2C polling error %d (transport errors: %lu).\n",
						transportError,
						static_cast<unsigned long>(pn532_i2c.getErrorCount()));
				lastLoggedError = transportError;
				lastErrorLogMs = now;
			}

			if (consecutiveNoTagPolls < 2) {
				++consecutiveNoTagPolls;
			}
			if (consecutiveNoTagPolls >= 2) {
				if (resetArmedUid.length() > 0 && !resetExecuted) {
					Serial.println("[NFC] Reset cancelled because the card was removed too soon.");
					tone(BUZZER_PIN, 300, 160);
				}
				presentedUid = "";
				resetArmedUid = "";
				resetArmedAtMs = 0;
				resetExecuted = false;
			}

			if (now - lastHealthLogMs >= 30000) {
				Serial.printf("[NFC] Polling health: %lu empty polls, %lu transport errors.\n",
						static_cast<unsigned long>(noTagPolls),
						static_cast<unsigned long>(pn532_i2c.getErrorCount()));
				noTagPolls = 0;
				lastHealthLogMs = now;
			}
		}

		vTaskDelay(pdMS_TO_TICKS(presentedUid.length() > 0 ? 100 : 250));
	}
}

void startNfcTask() {
	const BaseType_t result = xTaskCreatePinnedToCore(
		nfcTaskImpl, "NFCTask", 8192, NULL, 1, NULL, 0);
	setOtaNfcTaskRunning(result == pdPASS);
}

void setup() {
	Serial.begin(115200);
	// Keep ESP-IDF/Arduino core logs on USB Serial/JTAG instead of UART0.
	Serial.setDebugOutput(true);
	statusLedSetup();
	delay(1000);

	Serial.println("\n\n[Main] Booting ESP32-S3...");
	const esp_reset_reason_t resetReason = esp_reset_reason();
	Serial.printf("[Main] Reset reason: %s (%d)\n", resetReasonName(resetReason), resetReason);

	pinMode(BUZZER_PIN, OUTPUT);
	digitalWrite(BUZZER_PIN, LOW);
	pinMode(STATUS_LED_PIN, OUTPUT);
	digitalWrite(STATUS_LED_PIN, LOW);

	if (!Wire.setPins(SDA_PIN, SCL_PIN)) {
		Serial.println("[NFC] Failed to configure I2C pins.");
	}

	pendingMutex = xSemaphoreCreateMutex();
	initPersistentGameState();
	initRuntimeConfig();
	initNetworkConfig();

	nfc.begin();
	Wire.setClock(100000);
	Wire.setTimeOut(200);
	uint32_t versiondata = 0;
	for (uint8_t attempt = 1; attempt <= 3 && !versiondata; ++attempt) {
		versiondata = nfc.getFirmwareVersion();
		if (!versiondata && attempt < 3) {
			Serial.printf("[NFC] Firmware query failed (attempt %u/3); retrying.\n", attempt);
			delay(200);
		}
	}
	if (!versiondata) {
		Serial.println("[NFC] PN532 board not found via I2C");
		statusLedSetNfcAvailable(false);
	} else {
		statusLedSetNfcAvailable(true);
		Serial.printf("[NFC] Found chip PN5%02X, Firmware ver. %d.%d\n",
			(versiondata>>24) & 0xFF, (versiondata>>16) & 0xFF, (versiondata>>8) & 0xFF);
		bool samConfigured = false;
		for (uint8_t attempt = 1; attempt <= 3 && !samConfigured; ++attempt) {
			samConfigured = nfc.SAMConfig();
			if (!samConfigured && attempt < 3) delay(100);
		}
		if (!samConfigured) {
			// A busy PN532 can accept the command but miss the response deadline.
			// The pre-regression firmware started polling in this state and the
			// reader recovered on the next command, so do not disable NFC forever.
			Serial.printf("[NFC] SAMConfig response not confirmed (I2C error %d); continuing.\n",
					pn532_i2c.getLastError());
		}

		bool retriesConfigured = false;
		for (uint8_t attempt = 1; attempt <= 3 && !retriesConfigured; ++attempt) {
			retriesConfigured = nfc.setPassiveActivationRetries(0x01);
			if (!retriesConfigured && attempt < 3) delay(100);
		}
		if (retriesConfigured) {
			Serial.println("[NFC] Passive activation retries configured.");
		} else {
			Serial.printf("[NFC] Passive retry response not confirmed (I2C error %d); continuing.\n",
					pn532_i2c.getLastError());
		}

		startNfcTask();
	}

	initSubstations();
	networkSetup();
	setupOta();
	startNetworkTask();

	outChain = factory.createShiftRegisterChain(OUT_LATCH_PIN, OUT_DATA_PIN, SHARED_CLOCK_PIN);
	inChain = factory.createInputShiftRegisterChain(IN_LOAD_PIN, IN_DATA_PIN, SHARED_CLOCK_PIN, INPUT_REGISTER_COUNT);

	encoders.reserve(DEVICE_COUNT);
	buttons.reserve(DEVICE_COUNT);
	bargraphs.reserve(DEVICE_COUNT);

	const uint8_t encoderRegisterIndex[DEVICE_COUNT] = {0, 0, 1, 1, 2, 2};
	const uint8_t encoderBitPosition[DEVICE_COUNT]   = {0, 3, 0, 3, 0, 3};
	const uint8_t buttonRegisterIndex[DEVICE_COUNT]  = {0, 0, 1, 1, 2, 2};
	const uint8_t buttonBitPosition[DEVICE_COUNT]    = {2, 5, 2, 5, 2, 5};

	for (uint8_t i = 0; i < DEVICE_COUNT; ++i) {
		encoders.push_back(factory.createShiftEncoder(inChain, encoderRegisterIndex[i], encoderBitPosition[i], -10000, 10000, -1));
		buttons.push_back(factory.createShiftButton(inChain, buttonRegisterIndex[i], buttonBitPosition[i], true));
	}

	consumptionDisp = factory.createSegmentDisplay(outChain, DISPLAY_DIGIT_COUNT);
	productionDisp = factory.createSegmentDisplay(outChain, DISPLAY_DIGIT_COUNT);

	Bargraph* bg5 = factory.createBargraph(outChain, BARGRAPH_LED_COUNT);
	Bargraph* bg4 = factory.createBargraph(outChain, BARGRAPH_LED_COUNT);
	disp3 = factory.createSegmentDisplayPair(outChain, DISPLAY_DIGIT_COUNT);

	Bargraph* bg3 = factory.createBargraph(outChain, BARGRAPH_LED_COUNT);
	Bargraph* bg2 = factory.createBargraph(outChain, BARGRAPH_LED_COUNT);
	disp2 = factory.createSegmentDisplayPair(outChain, DISPLAY_DIGIT_COUNT);

	Bargraph* bg1 = factory.createBargraph(outChain, BARGRAPH_LED_COUNT);
	Bargraph* bg0 = factory.createBargraph(outChain, BARGRAPH_LED_COUNT);
	disp1 = factory.createSegmentDisplayPair(outChain, DISPLAY_DIGIT_COUNT);

	bargraphs.push_back(bg0);
	bargraphs.push_back(bg1);
	bargraphs.push_back(bg2);
	bargraphs.push_back(bg3);
	bargraphs.push_back(bg4);
	bargraphs.push_back(bg5);

	bargraphs[2]->setMode(BargraphMode::CENTER);

	coalDisplay = disp1.left();
	hydroDisplay = disp1.right();
	batteryDisplay = disp2.left();
	nuclearDisplay = disp2.right();
	gasDisplay = disp3.left();
	windPvDisplay = disp3.right();

}

void loop() {
	const uint32_t now = millis();
	const uint32_t nowUs = micros();
	if (!isOtaInProgress() &&
		static_cast<uint32_t>(nowUs - lastPeripheralUpdateUs) >= 1000) {
		lastPeripheralUpdateUs = nowUs;
		factory.update();
	}
	statusLedUpdate();
	handleOta();
	if (isOtaInProgress()) {
		// Keep the loop focused on the OTA server and status LED. In particular,
		// avoid substation I/O and display shifting while flash is being written.
		delay(1);
		return;
	}

	if (now - lastCountsUpdateMs >= 200) {
		lastCountsUpdateMs = now;
		updateTotalCounts();
	}

	if (now - lastDisplaysUpdateMs >= 20) {
		lastDisplaysUpdateMs = now;
		updateDisplays();
	}

	if (now - lastBargraphsUpdateMs >= 50) {
		lastBargraphsUpdateMs = now;
		updateBargraphs();
	}

	pollSubstations();
	queueSubstationUpdates();

	int32_t totalProdThisCycle = 0;

	for (size_t i = 0; i < DEVICE_COUNT; ++i) {
		if (i == 2 || i == 5) continue;
		uint8_t typeID = apiTypeMap[i];
		productionByTypeMW[typeID] = encoderValuesMW[i];
		totalProdThisCycle += encoderValuesMW[i];
	}

	int32_t sharedVal = encoderValuesMW[2];
	productionByTypeMW[8] = sharedVal / 2;
	productionByTypeMW[6] = sharedVal - productionByTypeMW[8];
	const int32_t storageConsumptionThisCycle = sharedVal < 0 ? -sharedVal : 0;
	if (sharedVal > 0) {
		totalProdThisCycle += sharedVal;
	}

	int32_t solarActiveMax = (int32_t)(baseMaxMW[1] * connectedCount[1] * currentCoefficient[1]);
	int32_t windActiveMax = (int32_t)(baseMaxMW[2] * connectedCount[2] * currentCoefficient[2]);

	productionByTypeMW[1] = solarActiveMax;
	productionByTypeMW[2] = windActiveMax;

	int32_t combinedWeatherPower = solarActiveMax + windActiveMax;
	totalProdThisCycle += combinedWeatherPower;

	encoderValuesMW[5] = combinedWeatherPower;
	if (encoders.size() > 5 && encoders[5]) {
		encoders[5]->set_value(combinedWeatherPower);
	}

	currentTotalProduction_MW = totalProdThisCycle;

	int32_t totalConsThisCycle = storageConsumptionThisCycle;
	for (int i = 0; i < BUILDING_COUNT; i++) {
		totalConsThisCycle += authoritativeBuildingCounts[i] * buildingConsumptionMW[i];
	}
	currentTotalConsumption_MW = totalConsThisCycle;

	//Serial.printf("[Main] Consumption: totalConsThisCycle=%u, currentTotalConsumption_MW=%u\n", totalConsThisCycle, currentTotalConsumption_MW);

	if (now - lastLedToggleMs >= 500) {
		lastLedToggleMs = now;
		ledState = !ledState;
		digitalWrite(STATUS_LED_PIN, ledState ? HIGH : LOW);
	}
}
