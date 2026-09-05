#ifndef OTAMANAGER_H
#define OTAMANAGER_H

#include <Arduino.h>

void setupOta();
void handleOta();
bool isOtaInProgress();
void setOtaDebugUpdateInProgress(bool active);
bool otaTasksPaused();
void setOtaNetworkTaskRunning(bool running);
void setOtaNetworkTaskPaused(bool paused);
void setOtaNfcTaskRunning(bool running);
void setOtaNfcTaskPaused(bool paused);
bool installPullFirmware(const String& apiBaseUrl, const String& jwtToken,
                         const String& jobId, const String& version,
                         uint32_t expectedSize, const String& expectedSha256,
                         const String& path);
String pullOtaJobId();
String pullOtaState();
String pullOtaError();

#endif
