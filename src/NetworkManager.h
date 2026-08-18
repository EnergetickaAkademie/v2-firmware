#ifndef NETWORKMANAGER_H
#define NETWORKMANAGER_H

#include <Arduino.h>

void initNetworkConfig();
void networkSetup();
void startNetworkTask();

bool queueWifiProvisioning(const String& ssid, const String& password, uint8_t security);
bool sendAddBuilding(uint8_t type, const String& uid);
void pollBuildingCounts();

#endif
