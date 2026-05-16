#ifndef SUBSTATION_MANAGER_H
#define SUBSTATION_MANAGER_H

#include <Arduino.h>

void initSubstations();

void pollSubstations();

void queueSubstationUpdates();

void updateTotalCounts();

#endif