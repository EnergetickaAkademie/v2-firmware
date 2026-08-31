#ifndef OTAMANAGER_H
#define OTAMANAGER_H

#include <Arduino.h>

void setupOta();
void handleOta();
bool isOtaInProgress();
bool installPullFirmware(const String& apiBaseUrl, const String& jwtToken,
                         const String& jobId, const String& version,
                         uint32_t expectedSize, const String& expectedSha256,
                         const String& path);
String pullOtaJobId();
String pullOtaState();
String pullOtaError();

#endif
