#ifndef NETWORKMANAGER_H
#define NETWORKMANAGER_H

#include <Arduino.h>

void networkSetup();
void startNetworkTask();

bool sendAddBuilding(uint8_t type, const String& uid);
void pollBuildingCounts();

#endif
