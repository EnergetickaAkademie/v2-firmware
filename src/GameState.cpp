#include "GameState.h"
#include "DebugLog.h"
#include <Preferences.h>

#define Serial DebugLog

String jwtToken = "";
int32_t currentTotalProduction_MW = 0;
int32_t currentTotalConsumption_MW = 0;

int32_t encoderValuesMW[6] = {0};
float encoderPercentages[6] = {0.0f};

int32_t baseMinMW[9] = {0};
int32_t baseMaxMW[9] = {0}; 
uint8_t connectedCount[9] = {0};
float currentCoefficient[9] = {0.0}; 
int32_t productionByTypeMW[9] = {0};

const uint8_t apiTypeMap[6] = {7, 5, 8, 3, 4, 2};

uint32_t buildingConsumptionMW[BUILDING_COUNT] = {0};
std::vector<ScannedBuilding> scannedBuildings;

uint8_t authoritativeBuildingCounts[BUILDING_COUNT] = {0};

std::vector<PendingBuilding> pendingBuildings;
SemaphoreHandle_t pendingMutex = nullptr;

namespace {
bool buildingScanningActive = false;
bool buildingResetPending = false;
bool buildingResetAcknowledged = false;
uint32_t buildingResetReadyAtMs = 0;

constexpr const char* BOARD_STATE_NAMESPACE = "board_state";
constexpr const char* RESET_PENDING_KEY = "reset_pending";
constexpr const char* NFC_OUTBOX_NAMESPACE = "nfc_outbox";

void persistPendingBuildingQueueLocked() {
	Preferences preferences;
	if (!preferences.begin(NFC_OUTBOX_NAMESPACE, false)) return;
	preferences.putUInt("count", pendingBuildings.size());
	for (size_t i = 0; i < pendingBuildings.size(); ++i) {
		String index = String(i);
		preferences.putString(("uid" + index).c_str(), pendingBuildings[i].uid);
		preferences.putUChar(("type" + index).c_str(), pendingBuildings[i].type);
		preferences.putUInt(("event" + index).c_str(), pendingBuildings[i].eventId);
	}
	preferences.end();
}

void persistResetPending(bool pending) {
	Preferences preferences;
	if (!preferences.begin(BOARD_STATE_NAMESPACE, false)) {
		Serial.println("[NFC] Failed to open persistent reset state.");
		return;
	}
	preferences.putBool(RESET_PENDING_KEY, pending);
	preferences.end();
}
}

void initPersistentGameState() {
	Preferences preferences;
	if (!preferences.begin(BOARD_STATE_NAMESPACE, true)) {
		Serial.println("[NFC] Failed to read persistent reset state.");
		return;
	}
	const bool pending = preferences.getBool(RESET_PENDING_KEY, false);
	preferences.end();

	Preferences outbox;
	uint32_t outboxCount = 0;
	if (!pending && outbox.begin(NFC_OUTBOX_NAMESPACE, true)) {
		outboxCount = outbox.getUInt("count", 0);
		if (outboxCount > 32) outboxCount = 0;
		for (uint32_t i = 0; i < outboxCount; ++i) {
			String index = String(i);
			String uid = outbox.getString(("uid" + index).c_str(), "");
			uint8_t type = outbox.getUChar(("type" + index).c_str(), 255);
			uint32_t eventId = outbox.getUInt(("event" + index).c_str(), 0);
			if (uid.length() > 0 && uid.length() <= 64 && type < BUILDING_COUNT) {
				scannedBuildings.push_back({uid, type});
				pendingBuildings.push_back(PendingBuilding(uid, type));
				pendingBuildings.back().eventId = eventId;
			}
		}
		outbox.end();
	}

	if (pendingMutex == nullptr ||
		xSemaphoreTake(pendingMutex, portMAX_DELAY) != pdTRUE) {
		return;
	}
	buildingResetPending = pending;
	if (pending) {
		memset(authoritativeBuildingCounts, 0, sizeof(authoritativeBuildingCounts));
		scannedBuildings.clear();
		pendingBuildings.clear();
	}
	xSemaphoreGive(pendingMutex);
	if (outboxCount > 0) {
		Serial.printf("[NFC] Restored %u MQTT building event(s) from NVS.\n",
			static_cast<unsigned>(outboxCount));
	}

	if (pending) {
		Serial.println("[NFC] Restored pending building reset; server update will retry after reconnect.");
	}
}

BuildingScanQueueResult queueBuildingScan(const String& uid, uint8_t type) {
	if (pendingMutex == nullptr) {
		return BuildingScanQueueResult::Unavailable;
	}

	if (xSemaphoreTake(pendingMutex, portMAX_DELAY) != pdTRUE) {
		return BuildingScanQueueResult::Unavailable;
	}

	BuildingScanQueueResult result = BuildingScanQueueResult::Queued;
	if (buildingResetPending ||
		static_cast<int32_t>(millis() - buildingResetReadyAtMs) < 0) {
		result = BuildingScanQueueResult::ResetPending;
	} else if (!buildingScanningActive) {
		result = BuildingScanQueueResult::Inactive;
	} else {
		for (const auto& building : scannedBuildings) {
			if (building.uid == uid) {
				result = BuildingScanQueueResult::Duplicate;
				break;
			}
		}

		if (result == BuildingScanQueueResult::Queued) {
			scannedBuildings.push_back({uid, type});
			pendingBuildings.push_back(PendingBuilding(uid, type));
			persistPendingBuildingQueueLocked();
		}
	}

	xSemaphoreGive(pendingMutex);
	return result;
}

bool hasBuildingBeenScanned(const String& uid) {
	if (pendingMutex == nullptr ||
		xSemaphoreTake(pendingMutex, portMAX_DELAY) != pdTRUE) {
		return false;
	}

	bool duplicate = false;
	for (const auto& building : scannedBuildings) {
		if (building.uid == uid) {
			duplicate = true;
			break;
		}
	}

	xSemaphoreGive(pendingMutex);
	return duplicate;
}

void setBuildingScanScenarioState(bool active, bool resetCache) {
	if (pendingMutex == nullptr ||
		xSemaphoreTake(pendingMutex, portMAX_DELAY) != pdTRUE) {
		return;
	}

	buildingScanningActive = active;
	bool clearedState = false;
	if (resetCache) {
		clearedState = !scannedBuildings.empty() || !pendingBuildings.empty();
		scannedBuildings.clear();
		pendingBuildings.clear();
	}

	xSemaphoreGive(pendingMutex);

	if (clearedState) {
		persistPendingBuildingQueue();
		Serial.println("[NFC] Cleared local scan state at scenario boundary.");
	}
}

void requestBuildingReset() {
	if (pendingMutex == nullptr ||
		xSemaphoreTake(pendingMutex, portMAX_DELAY) != pdTRUE) {
		return;
	}

	buildingResetPending = true;
	buildingResetAcknowledged = false;
	buildingResetReadyAtMs = 0;
	scannedBuildings.clear();
	pendingBuildings.clear();
	memset(authoritativeBuildingCounts, 0, sizeof(authoritativeBuildingCounts));
	xSemaphoreGive(pendingMutex);
	persistPendingBuildingQueue();

	persistResetPending(true);
	Serial.println("[NFC] Building reset accepted locally; waiting for server confirmation.");
}

bool isBuildingResetPending() {
	if (pendingMutex == nullptr ||
		xSemaphoreTake(pendingMutex, portMAX_DELAY) != pdTRUE) {
		return false;
	}
	const bool pending = buildingResetPending;
	xSemaphoreGive(pendingMutex);
	return pending;
}

void markBuildingResetCompleted() {
	if (pendingMutex == nullptr ||
		xSemaphoreTake(pendingMutex, portMAX_DELAY) != pdTRUE) {
		return;
	}
	buildingResetPending = false;
	buildingResetAcknowledged = true;
	// Give the next sync response time to deliver the authoritative zero
	// counts before accepting new building scans.
	buildingResetReadyAtMs = millis() + 1000;
	xSemaphoreGive(pendingMutex);

	persistResetPending(false);
}

bool consumeBuildingResetAcknowledged() {
	if (pendingMutex == nullptr ||
		xSemaphoreTake(pendingMutex, portMAX_DELAY) != pdTRUE) {
		return false;
	}
	const bool acknowledged = buildingResetAcknowledged;
	buildingResetAcknowledged = false;
	xSemaphoreGive(pendingMutex);
	return acknowledged;
}

void persistPendingBuildingQueue() {
	if (pendingMutex == nullptr || xSemaphoreTake(pendingMutex, portMAX_DELAY) != pdTRUE) return;
	persistPendingBuildingQueueLocked();
	xSemaphoreGive(pendingMutex);
}
