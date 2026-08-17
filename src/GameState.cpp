#include "GameState.h"

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
}

BuildingScanQueueResult queueBuildingScan(const String& uid, uint8_t type) {
	if (pendingMutex == nullptr) {
		return BuildingScanQueueResult::Unavailable;
	}

	if (xSemaphoreTake(pendingMutex, portMAX_DELAY) != pdTRUE) {
		return BuildingScanQueueResult::Unavailable;
	}

	BuildingScanQueueResult result = BuildingScanQueueResult::Queued;
	if (!buildingScanningActive) {
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
			pendingBuildings.push_back({uid, type});
		}
	}

	xSemaphoreGive(pendingMutex);
	return result;
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
		Serial.println("[NFC] Cleared local scan state at scenario boundary.");
	}
}