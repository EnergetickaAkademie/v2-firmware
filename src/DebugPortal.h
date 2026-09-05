#ifndef DEBUG_PORTAL_H
#define DEBUG_PORTAL_H

#include <Arduino.h>

void requestDebugPortal();
void requestDebugPortalExit();
bool isDebugPortalRequested();
bool isDebugPortalActive();
void handleDebugPortal();

#endif
