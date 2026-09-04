#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include <Arduino.h>

constexpr uint8_t DEVICE_CONFIG_SCHEMA = 1;

void initRuntimeConfig();
bool runtimeConfigReady();
uint8_t runtimeConfigSchema();
const String& runtimeApiBaseUrl();
const String& runtimeBoardUsername();
const String& runtimeBoardPassword();
const String& runtimeOtaPassword();
const String& runtimeOtaHostname();
uint16_t runtimeOtaPort();

bool saveRuntimeConfig(const String& apiUrl, const String& boardUsername,
	const String& boardPassword, const String& otaPassword,
	const String& otaHostname, uint16_t otaPort);

#endif
