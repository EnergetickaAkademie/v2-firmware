#ifndef GAMESTATE_H
#define GAMESTATE_H

#include <Arduino.h>
#include <vector>
#include "BuildingTypes.h"


extern String jwtToken;
extern int32_t currentTotalProduction_MW;
extern int32_t currentTotalConsumption_MW;

extern int32_t encoderValuesMW[6];
extern float encoderPercentages[6];

extern int32_t baseMinMW[9];
extern int32_t baseMaxMW[9];
extern uint8_t connectedCount[9];
extern float currentCoefficient[9];
extern int32_t productionByTypeMW[9];

extern const uint8_t apiTypeMap[6];

extern uint32_t buildingConsumptionMW[BUILDING_COUNT];

struct ScannedBuilding {
	String uid;
	uint8_t type;
};
extern std::vector<ScannedBuilding> scannedBuildings;

extern uint8_t authoritativeBuildingCounts[BUILDING_COUNT];

struct PendingBuilding {
	String uid;
	uint8_t type;
};
extern std::vector<PendingBuilding> pendingBuildings;
extern SemaphoreHandle_t pendingMutex;

#endif