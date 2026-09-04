#ifndef SUBSTATION_MANAGER_H
#define SUBSTATION_MANAGER_H

#include <Arduino.h>

void initSubstations();

void pollSubstations();

void queueSubstationUpdates();

void updateTotalCounts();

uint8_t getOnlineSubstationCount();

struct SubstationSnapshot {
    bool online;
    uint32_t lastSeenAgeMs;
    uint8_t counts[8];
};

void getSubstationSnapshots(SubstationSnapshot snapshots[3]);

#endif
