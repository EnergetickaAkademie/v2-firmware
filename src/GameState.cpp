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