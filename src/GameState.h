#ifndef GAMESTATE_H
#define GAMESTATE_H

#include <Arduino.h>
#include <vector>

extern String jwtToken;
extern uint32_t currentTotalProduction_mW;
extern uint32_t currentTotalConsumption_mW;

extern int32_t encoderValuesMW[6];
extern float encoderPercentages[6];

extern int32_t baseMinMW[9];
extern int32_t baseMaxMW[9];
extern uint8_t connectedCount[9];
extern float currentCoefficient[9];
extern int32_t productionByTypeMW[9];

extern const uint8_t apiTypeMap[6];

// New consumption tracking variables
extern uint32_t buildingConsumptionMW[0x12];
extern uint8_t buildingCounts[0x12];

struct ScannedBuilding {
    String uid;
    uint8_t type;
};
extern std::vector<ScannedBuilding> scannedBuildings;

#endif