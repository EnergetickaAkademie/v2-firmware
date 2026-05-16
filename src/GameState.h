#ifndef GAMESTATE_H
#define GAMESTATE_H

#include <Arduino.h>

extern String jwtToken;
extern uint32_t currentTotalProduction_mW;
extern uint32_t currentTotalConsumption_mW;

extern int32_t encoderValuesMW[6];

extern int32_t baseMinMW[9];
extern int32_t baseMaxMW[9];
extern uint8_t connectedCount[9];
extern float currentCoefficient[9];
extern int32_t productionByTypeMW[9];

extern const uint8_t apiTypeMap[6];

#endif