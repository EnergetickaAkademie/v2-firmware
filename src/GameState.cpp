#include "GameState.h"

String jwtToken = "";
uint32_t currentTotalProduction_mW = 0;
uint32_t currentTotalConsumption_mW = 0;

int32_t encoderValuesMW[6] = {0};

int32_t baseMinMW[9] = {0};
int32_t baseMaxMW[9] = {0}; 
uint8_t connectedCount[9] = {0};
float currentCoefficient[9] = {0.0}; 
int32_t productionByTypeMW[9] = {0};

const uint8_t apiTypeMap[6] = {7, 5, 8, 3, 4, 2};