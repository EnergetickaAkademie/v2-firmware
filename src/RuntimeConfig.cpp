#include "RuntimeConfig.h"

#include <Preferences.h>

#include "Config.h"
#include "DebugLog.h"

#define Serial DebugLog

namespace {
Preferences preferences;
String apiUrl;
String boardUsername;
String boardPassword;
String otaPassword;
String otaHostname;
uint16_t otaPort = OTA_PORT;
uint8_t schema = 0;
bool ready = false;

bool validValue(const String& value) {
    return value.length() > 0 && value.length() < 512;
}
}

void initRuntimeConfig() {
    if (!preferences.begin("device", false)) {
        Serial.println("[Config] NVS unavailable; runtime configuration is not ready.");
        return;
    }

    schema = preferences.getUChar("schema", 0);
    if (schema < DEVICE_CONFIG_SCHEMA) {
        const bool usableDefaults = String(BOARD_USERNAME) != "__UNPROVISIONED__" &&
            String(BOARD_PASSWORD) != "__UNPROVISIONED__" &&
            String(API_BASE_URL) != "__UNPROVISIONED__" &&
            String(OTA_PASSWORD) != "CHANGE_ME" && strlen(OTA_PASSWORD) >= 8;
        if (usableDefaults) {
            preferences.putString("api", API_BASE_URL);
            preferences.putString("user", BOARD_USERNAME);
            preferences.putString("pass", BOARD_PASSWORD);
            preferences.putString("ota", OTA_PASSWORD);
            preferences.putString("host", OTA_HOSTNAME);
            preferences.putUShort("port", OTA_PORT);
            preferences.putUChar("schema", DEVICE_CONFIG_SCHEMA);
            schema = DEVICE_CONFIG_SCHEMA;
            Serial.println("[Config] Migrated compile-time device settings to NVS.");
        }
    }

    apiUrl = preferences.getString("api", "");
    boardUsername = preferences.getString("user", "");
    boardPassword = preferences.getString("pass", "");
    otaPassword = preferences.getString("ota", "");
    otaHostname = preferences.getString("host", OTA_HOSTNAME);
    otaPort = preferences.getUShort("port", OTA_PORT);
    ready = schema >= DEVICE_CONFIG_SCHEMA && validValue(apiUrl) &&
        validValue(boardUsername) && validValue(boardPassword) &&
        otaPassword.length() >= 8 && otaHostname.length() > 0 &&
        otaPort > 0 && otaPort != 80;
    if (!ready) Serial.println("[Config] Device is not provisioned for generic OTA releases.");
}

bool runtimeConfigReady() { return ready; }
uint8_t runtimeConfigSchema() { return schema; }
const String& runtimeApiBaseUrl() { return apiUrl; }
const String& runtimeBoardUsername() { return boardUsername; }
const String& runtimeBoardPassword() { return boardPassword; }
const String& runtimeOtaPassword() { return otaPassword; }
const String& runtimeOtaHostname() { return otaHostname; }
uint16_t runtimeOtaPort() { return otaPort; }

bool saveRuntimeConfig(const String& nextApiUrl, const String& nextBoardUsername,
                      const String& nextBoardPassword, const String& nextOtaPassword,
                      const String& nextOtaHostname, uint16_t nextOtaPort) {
    if (!validValue(nextApiUrl) || !validValue(nextBoardUsername) ||
        !validValue(nextBoardPassword) || nextOtaPassword.length() < 8 ||
        nextOtaPassword.length() > 127 || nextOtaHostname.length() == 0 ||
        nextOtaHostname.length() > 63 || nextOtaPort == 0 || nextOtaPort == 80) {
        return false;
    }

    bool ok = preferences.putString("api", nextApiUrl) > 0;
    ok = ok && preferences.putString("user", nextBoardUsername) > 0;
    ok = ok && preferences.putString("pass", nextBoardPassword) > 0;
    ok = ok && preferences.putString("ota", nextOtaPassword) > 0;
    ok = ok && preferences.putString("host", nextOtaHostname) > 0;
    ok = ok && preferences.putUShort("port", nextOtaPort) > 0;
    if (ok) {
        ok = preferences.putUChar("schema", DEVICE_CONFIG_SCHEMA) > 0;
    }
    if (!ok) {
        Serial.println("[Config] Failed to persist debug-portal configuration.");
        return false;
    }

    apiUrl = nextApiUrl;
    boardUsername = nextBoardUsername;
    boardPassword = nextBoardPassword;
    otaPassword = nextOtaPassword;
    otaHostname = nextOtaHostname;
    otaPort = nextOtaPort;
    schema = DEVICE_CONFIG_SCHEMA;
    ready = true;
    return true;
}
