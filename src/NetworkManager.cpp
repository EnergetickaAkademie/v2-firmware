#include "NetworkManager.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <vector>
#include "Config.h"
#include "DebugLog.h"
#include "GameState.h"
#include "OtaManager.h"
#include "RuntimeConfig.h"
#include "StatusLedManager.h"
#include "MqttTransport.h"

#define Serial DebugLog

unsigned long lastNetworkRetry = 0;
const unsigned long NETWORK_RETRY_INTERVAL = 5000;

unsigned long lastPollMs = 0;
const unsigned long POLL_INTERVAL = 2000;
unsigned long lastPostMs = 0;
const unsigned long POST_INTERVAL = 1000;
unsigned long lastSyncMs = 0;
const unsigned long SYNC_INTERVAL = 500;
unsigned long lastFirmwareSyncMs = 0;
const unsigned long FIRMWARE_SYNC_INTERVAL = 2000;

namespace {
constexpr uint32_t STREAM_READ_TIMEOUT_MS = 3000;
constexpr size_t SYNC_V2_REQUEST_SIZE = 52;
constexpr size_t SYNC_V2_RESPONSE_SIZE = 210;
constexpr uint32_t SYNC_V2_RETRY_INTERVAL_MS = 60000;
constexpr uint32_t WIFI_CANDIDATE_TIMEOUT_MS = 30000;
constexpr uint32_t BUILDING_RESET_RETRY_MS = 2000;
bool boardRegistered = false;
bool syncV2Available = true;
bool firmwareModeActive = false;
volatile bool debugNetworkMode = false;
uint32_t syncV2RetryAt = 0;
uint32_t syncSequence = 0;
uint32_t lastConfigRevision = 0;
String mqttBootstrapUri;
uint32_t mqttBootstrapRetryAt = 0;
bool mqttBootstrapAttempted = false;
uint32_t mqttDisconnectedSince = 0;

bool handleAuthFailure(int httpCode, const char* endpoint);
void logHttpFailure(const char* endpoint,
                    int httpCode,
                    StatusApiError statusError = StatusApiError::None);

Preferences wifiPreferences;
SemaphoreHandle_t wifiConfigMutex = nullptr;
bool wifiPreferencesReady = false;
String activeWifiSsid = WIFI_SSID;
String activeWifiPassword = WIFI_PASS;
uint8_t activeWifiSecurity = 1;
String candidateWifiSsid;
String candidateWifiPassword;
uint8_t candidateWifiSecurity = 1;
bool candidateWifiPending = false;
uint32_t candidateWifiGeneration = 0;
bool candidateWifiAttemptActive = false;
uint32_t candidateWifiAttemptGeneration = 0;
uint32_t candidateWifiAttemptStartedAt = 0;

enum class WiFiSetupPhase {
    Uninitialized,
    WaitingToPowerOff,
    WaitingToStartStation,
    Ready,
};

WiFiSetupPhase wifiSetupPhase = WiFiSetupPhase::Uninitialized;
uint32_t wifiSetupPhaseStartedAt = 0;

enum class SyncV2Result {
    Success,
    NotSupported,
    Failed,
};

void addBoardMetadata(HTTPClient& http) {
    http.addHeader("X-ENAK-Firmware-Version", FIRMWARE_VERSION);
    http.addHeader("X-ENAK-OTA-Port", String(runtimeOtaPort()));
    http.addHeader("X-ENAK-OTA-Ready", runtimeConfigReady() ? "1" : "0");
    http.addHeader("X-ENAK-Config-Schema", String(runtimeConfigSchema()));
}

bool mqttV3Enabled() {
    return ENAK_MQTT_V3_ENABLED != 0;
}

bool bootstrapMqtt(uint32_t now) {
    if (!mqttV3Enabled() || jwtToken == "" || !boardRegistered ||
        WiFi.status() != WL_CONNECTED ||
        static_cast<int32_t>(now - mqttBootstrapRetryAt) < 0) return false;

    // A successful bootstrap owns a reconnecting esp-mqtt client. Do not tear
    // that client down and repeat the HTTPS bootstrap every 30 seconds.
    if (mqttBootstrapAttempted) return mqttTransportConnected();

    HTTPClient http;
    const String bootstrapUrl = runtimeApiBaseUrl() + "/board/bootstrap/v3";
    Serial.printf("[MQTT] Requesting bootstrap from %s.\n", bootstrapUrl.c_str());
    http.begin(bootstrapUrl);
    http.addHeader("Authorization", "Bearer " + jwtToken);
    const int code = http.GET();
    if (code == HTTP_CODE_OK) {
        JsonDocument document;
        const DeserializationError error = deserializeJson(document, http.getString());
        const String uri = error ? String() : document["mqtt_uri"].as<String>();
        const String root = error ? String() : document["topic_root"].as<String>();
        const int protocol = error ? 0 : document["protocol"].as<int>();
        if (!error && protocol == 3 && uri.startsWith("ws") && root.length() > 0) {
            Serial.printf("[MQTT] Bootstrap accepted: protocol=%d uri=%s topic=%s.\n",
                          protocol, uri.c_str(), root.c_str());
            mqttBootstrapUri = uri;
            mqttBootstrapAttempted = mqttTransportStart(mqttBootstrapUri);
            mqttBootstrapRetryAt = now + (mqttBootstrapAttempted ? 30000 : 5000);
            http.end();
            return mqttBootstrapAttempted;
        }
        Serial.println("[MQTT] Bootstrap response was incomplete or unusable.");
        statusLedRecordApiFailure(StatusApiError::InvalidResponse);
        mqttBootstrapRetryAt = now + 30000;
    } else {
        if (handleAuthFailure(code, "/board/bootstrap/v3")) {
            http.end();
            return false;
        }
        logHttpFailure("/board/bootstrap/v3", code);
        mqttBootstrapRetryAt = now + (code == HTTP_CODE_SERVICE_UNAVAILABLE ? 5000 : 30000);
    }
    http.end();
    return false;
}

void beginWifiConnection(const String& ssid, const String& password, uint8_t security) {
    if (security == 0) {
        WiFi.begin(ssid.c_str());
    } else {
        WiFi.begin(ssid.c_str(), password.c_str());
    }
}

void clearAuthenticationState() {
    jwtToken = "";
    boardRegistered = false;
    statusLedSetBoardRegistered(false);
    mqttTransportStop();
    mqttBootstrapAttempted = false;
}

bool readCandidateWifi(String& ssid, String& password, uint8_t& security,
                       uint32_t& generation) {
    if (wifiConfigMutex == nullptr ||
        xSemaphoreTake(wifiConfigMutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool pending = candidateWifiPending;
    if (pending) {
        ssid = candidateWifiSsid;
        password = candidateWifiPassword;
        security = candidateWifiSecurity;
        generation = candidateWifiGeneration;
    }
    xSemaphoreGive(wifiConfigMutex);
    return pending;
}

void startCandidateWifiAttempt(const String& ssid, const String& password,
                               uint8_t security, uint32_t generation,
                               uint32_t now) {
    Serial.printf("[WiFi] Testing NFC-provisioned network: %s\n", ssid.c_str());
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    beginWifiConnection(ssid, password, security);
    clearAuthenticationState();
    candidateWifiAttemptActive = true;
    candidateWifiAttemptGeneration = generation;
    candidateWifiAttemptStartedAt = now;
    lastNetworkRetry = now;
}

bool processCandidateWifi(uint32_t now) {
    String ssid;
    String password;
    uint8_t security = 1;
    uint32_t generation = 0;
    const bool pending = readCandidateWifi(ssid, password, security, generation);

    if (!pending) {
        candidateWifiAttemptActive = false;
        return false;
    }

    if (!candidateWifiAttemptActive || generation != candidateWifiAttemptGeneration) {
        startCandidateWifiAttempt(ssid, password, security, generation, now);
        return true;
    }

    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) {
        if (wifiConfigMutex != nullptr &&
            xSemaphoreTake(wifiConfigMutex, portMAX_DELAY) == pdTRUE) {
            if (candidateWifiPending &&
                candidateWifiGeneration == candidateWifiAttemptGeneration) {
                activeWifiSsid = candidateWifiSsid;
                activeWifiPassword = candidateWifiPassword;
                activeWifiSecurity = candidateWifiSecurity;
                candidateWifiPending = false;
                wifiPreferences.putString("ssid", activeWifiSsid);
                wifiPreferences.putString("password", activeWifiPassword);
                wifiPreferences.putUChar("security", activeWifiSecurity);
                wifiPreferences.remove("candidate_ssid");
                wifiPreferences.remove("candidate_pass");
                wifiPreferences.remove("candidate_sec");
                wifiPreferences.putBool("candidate", false);
            }
            xSemaphoreGive(wifiConfigMutex);
        }
        WiFi.setAutoReconnect(true);
        candidateWifiAttemptActive = false;
        Serial.printf("[WiFi] NFC-provisioned network confirmed and saved: %s\n",
                      activeWifiSsid.c_str());
        return false;
    }

    if (now - candidateWifiAttemptStartedAt < WIFI_CANDIDATE_TIMEOUT_MS) {
        return true;
    }

    if (wifiConfigMutex != nullptr &&
        xSemaphoreTake(wifiConfigMutex, portMAX_DELAY) == pdTRUE) {
        if (candidateWifiGeneration == candidateWifiAttemptGeneration) {
            candidateWifiPending = false;
            wifiPreferences.remove("candidate_ssid");
            wifiPreferences.remove("candidate_pass");
            wifiPreferences.remove("candidate_sec");
            wifiPreferences.putBool("candidate", false);
        }
        xSemaphoreGive(wifiConfigMutex);
    }

    Serial.printf("[WiFi] NFC-provisioned network failed; restoring: %s\n",
                  activeWifiSsid.c_str());
    WiFi.disconnect(false, false);
    WiFi.setAutoReconnect(true);
    beginWifiConnection(activeWifiSsid, activeWifiPassword, activeWifiSecurity);
    candidateWifiAttemptActive = false;
    lastNetworkRetry = now;
    return true;
}

bool readExact(WiFiClient* stream, uint8_t* buffer, size_t length) {
    if (!stream || !buffer) return false;
    stream->setTimeout(STREAM_READ_TIMEOUT_MS);
    size_t read = stream->readBytes(reinterpret_cast<char*>(buffer), length);
    if (read != length) {
        Serial.printf("[Net] Short HTTP body: expected %u bytes, got %u\n",
                      static_cast<unsigned>(length),
                      static_cast<unsigned>(read));
        return false;
    }
    return true;
}

bool readByte(WiFiClient* stream, uint8_t& value) {
    return readExact(stream, &value, 1);
}

bool readBE32(WiFiClient* stream, int32_t& value) {
    uint8_t buf[4];
    if (!readExact(stream, buf, sizeof(buf))) return false;

    uint32_t raw =
        (static_cast<uint32_t>(buf[0]) << 24) |
        (static_cast<uint32_t>(buf[1]) << 16) |
        (static_cast<uint32_t>(buf[2]) << 8) |
        static_cast<uint32_t>(buf[3]);
    value = static_cast<int32_t>(raw);
    return true;
}

bool handleAuthFailure(int httpCode, const char* endpoint) {
    if (httpCode == 401 || httpCode == 403) {
        Serial.printf("[Net] %s rejected token (HTTP %d); re-authentication required.\n",
                      endpoint, httpCode);
        jwtToken = "";
        boardRegistered = false;
        statusLedSetBoardRegistered(false);
        statusLedRecordApiFailure(StatusApiError::Authentication);
        return true;
    }
    return false;
}

bool handleBoardNotFound(HTTPClient& http, int httpCode, const char* endpoint) {
    if (httpCode != HTTP_CODE_NOT_FOUND) return false;

    String body = http.getString();
    body.trim();
    if (body != "BOARD_NOT_FOUND") return false;

    Serial.printf("[Net] %s no longer recognizes this board; re-registration required.\n",
                  endpoint);
    boardRegistered = false;
    statusLedSetBoardRegistered(false);
    statusLedRecordApiFailure(StatusApiError::Registration);
    return true;
}

void writeBEU32(uint8_t* destination, uint32_t raw) {
    destination[0] = static_cast<uint8_t>(raw >> 24);
    destination[1] = static_cast<uint8_t>(raw >> 16);
    destination[2] = static_cast<uint8_t>(raw >> 8);
    destination[3] = static_cast<uint8_t>(raw);
}

void writeBE32(uint8_t* destination, int32_t value) {
    writeBEU32(destination, static_cast<uint32_t>(value));
}

uint32_t readBEU32(const uint8_t* source) {
    return (static_cast<uint32_t>(source[0]) << 24) |
           (static_cast<uint32_t>(source[1]) << 16) |
           (static_cast<uint32_t>(source[2]) << 8) |
           static_cast<uint32_t>(source[3]);
}

int32_t readBE32(const uint8_t* source) {
    return static_cast<int32_t>(readBEU32(source));
}

void logHttpFailure(const char* endpoint,
                    int httpCode,
                    StatusApiError statusError) {
    if (httpCode < 0) {
        Serial.printf("[Net] %s failed: %s (%d)\n",
                      endpoint,
                      HTTPClient::errorToString(httpCode).c_str(),
                      httpCode);
    } else {
        Serial.printf("[Net] %s failed: HTTP %d\n", endpoint, httpCode);
    }

    if (statusError == StatusApiError::None) {
        statusError = (httpCode < 0 || httpCode >= 500)
                          ? StatusApiError::Unreachable
                          : StatusApiError::InvalidResponse;
    }
    statusLedRecordApiFailure(statusError);
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            statusLedSetWifiConnected(true);
            Serial.printf("[WiFi] Connected. IP=%s RSSI=%d dBm\n",
                          WiFi.localIP().toString().c_str(),
                          WiFi.RSSI());
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            statusLedSetWifiConnected(false);
            Serial.printf("[WiFi] Disconnected. reason=%d\n",
                          info.wifi_sta_disconnected.reason);
            break;
        default:
            break;
    }
}

void processPendingBuildingUpload(uint32_t now) {
    static uint32_t lastAttemptMs = 0;
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED || pendingMutex == nullptr) return;
    if (isBuildingResetPending()) return;
    if (now - lastAttemptMs < 2000) return;

    PendingBuilding pending("", 0);
    bool hasPending = false;

    if (xSemaphoreTake(pendingMutex, 0) == pdTRUE) {
        if (!pendingBuildings.empty()) {
            pending = pendingBuildings.front();
            hasPending = true;
        }
        xSemaphoreGive(pendingMutex);
    }

    if (!hasPending) return;

    lastAttemptMs = now;
    if (!sendAddBuilding(pending.type, pending.uid)) {
        Serial.println("[Net] Building upload failed; keeping it queued for retry.");
        return;
    }

    bool removed = false;
    if (xSemaphoreTake(pendingMutex, portMAX_DELAY) == pdTRUE) {
        if (!pendingBuildings.empty() &&
            pendingBuildings.front().uid == pending.uid &&
            pendingBuildings.front().type == pending.type) {
            pendingBuildings.erase(pendingBuildings.begin());
            removed = true;
        }
        xSemaphoreGive(pendingMutex);
    }
    if (removed) persistPendingBuildingQueue();

    Serial.println("[Net] Queued building confirmed by server.");
}

bool processPendingBuildingReset(uint32_t now) {
    static uint32_t lastAttemptMs = 0;
    if (!isBuildingResetPending()) return false;
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED || !boardRegistered) return true;
    if (now - lastAttemptMs < BUILDING_RESET_RETRY_MS) return true;
    lastAttemptMs = now;

    HTTPClient http;
    http.begin(runtimeApiBaseUrl() + "/board/reset_buildings");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    int code = http.POST("");
    const bool success = code >= 200 && code < 300;
    if (!success) {
        if (!handleAuthFailure(code, "/board/reset_buildings") &&
            !handleBoardNotFound(http, code, "/board/reset_buildings")) {
            logHttpFailure("/board/reset_buildings", code);
        }
        http.end();
        return true;
    }

    http.end();
    markBuildingResetCompleted();
    statusLedRecordApiSuccess();
    Serial.println("[Net] Building reset confirmed by server.");
    return false;
}

bool advanceWiFiSetup(uint32_t now) {
    if (wifiSetupPhase == WiFiSetupPhase::WaitingToPowerOff &&
        now - wifiSetupPhaseStartedAt >= 1000) {
        Serial.println("[Net] Setting WiFi OFF mode");
        WiFi.mode(WIFI_OFF);
        wifiSetupPhase = WiFiSetupPhase::WaitingToStartStation;
        wifiSetupPhaseStartedAt = now;
    }

    if (wifiSetupPhase == WiFiSetupPhase::WaitingToStartStation &&
        now - wifiSetupPhaseStartedAt >= 1000) {
        Serial.println("[Net] Setting Station Mode...");
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);

        Serial.printf("[Net] Connecting to WiFi: %s\n", activeWifiSsid.c_str());
        beginWifiConnection(activeWifiSsid, activeWifiPassword, activeWifiSecurity);
        lastNetworkRetry = now;
        wifiSetupPhase = WiFiSetupPhase::Ready;
    }

    return wifiSetupPhase == WiFiSetupPhase::Ready;
}
} // namespace

void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.printf("[Net] Reconnecting to WiFi: %s\n", activeWifiSsid.c_str());
    beginWifiConnection(activeWifiSsid, activeWifiPassword, activeWifiSecurity);
}

void enterDebugNetworkMode() {
    debugNetworkMode = true;
    jwtToken = "";
    boardRegistered = false;
    statusLedSetBoardRegistered(false);
    statusLedSetWifiConnected(false);
    statusLedSetMqttHealthy(false);
    mqttTransportStop();
    mqttBootstrapAttempted = false;
    WiFi.disconnect(false, false);
}

void leaveDebugNetworkMode() { debugNetworkMode = false; }

bool isDebugNetworkMode() { return debugNetworkMode; }

void initNetworkConfig() {
    wifiConfigMutex = xSemaphoreCreateMutex();
    if (wifiConfigMutex == nullptr) {
        Serial.println("[WiFi] Configuration lock unavailable; NFC provisioning disabled.");
        return;
    }
    wifiPreferencesReady = wifiPreferences.begin("network", false);
    if (!wifiPreferencesReady) {
        Serial.println("[WiFi] Persistent configuration unavailable; using firmware defaults.");
        return;
    }

    activeWifiSsid = wifiPreferences.getString("ssid", WIFI_SSID);
    activeWifiPassword = wifiPreferences.getString("password", WIFI_PASS);
    activeWifiSecurity = wifiPreferences.getUChar(
        "security", activeWifiPassword.length() == 0 ? 0 : 1);
    // Preserve compile-time Wi-Fi settings across the first generic OTA
    // upgrade. Never overwrite credentials provisioned through NFC.
    if (!wifiPreferences.isKey("ssid") && activeWifiSsid.length() > 0) {
        wifiPreferences.putString("ssid", activeWifiSsid);
        wifiPreferences.putString("password", activeWifiPassword);
        wifiPreferences.putUChar("security", activeWifiSecurity);
    }

    candidateWifiPending = wifiPreferences.getBool("candidate", false);
    if (candidateWifiPending) {
        candidateWifiSsid = wifiPreferences.getString("candidate_ssid", "");
        candidateWifiPassword = wifiPreferences.getString("candidate_pass", "");
        candidateWifiSecurity = wifiPreferences.getUChar("candidate_sec", 1);
        if (candidateWifiSsid.length() == 0) {
            candidateWifiPending = false;
            wifiPreferences.putBool("candidate", false);
        } else {
            candidateWifiGeneration = 1;
            Serial.println("[WiFi] Restored an interrupted NFC provisioning attempt.");
        }
    }

    Serial.printf("[WiFi] Active network configuration: %s\n", activeWifiSsid.c_str());
}

bool queueWifiProvisioning(const String& ssid, const String& password, uint8_t security) {
    if (ssid.length() == 0 || ssid.length() > 32 || security > 1 ||
        (security == 0 && password.length() != 0) ||
        (security == 1 && (password.length() < 8 || password.length() > 63)) ||
        wifiConfigMutex == nullptr || !wifiPreferencesReady) {
        return false;
    }

    if (xSemaphoreTake(wifiConfigMutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    candidateWifiSsid = ssid;
    candidateWifiPassword = password;
    candidateWifiSecurity = security;
    candidateWifiPending = true;
    ++candidateWifiGeneration;
    wifiPreferences.putString("candidate_ssid", candidateWifiSsid);
    wifiPreferences.putString("candidate_pass", candidateWifiPassword);
    wifiPreferences.putUChar("candidate_sec", candidateWifiSecurity);
    wifiPreferences.putBool("candidate", true);
    xSemaphoreGive(wifiConfigMutex);

    Serial.printf("[WiFi] NFC provisioning queued for SSID: %s\n", ssid.c_str());
    return true;
}

bool getActiveWifiConfig(ActiveWifiConfig& config) {
    if (!wifiPreferencesReady || wifiConfigMutex == nullptr ||
        xSemaphoreTake(wifiConfigMutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    config.ssid = activeWifiSsid;
    config.security = activeWifiSecurity;
    config.passwordSet = activeWifiPassword.length() > 0;
    xSemaphoreGive(wifiConfigMutex);
    return true;
}

void networkSetup() {
    WiFi.onEvent(onWiFiEvent);
    statusLedSetWifiConnected(false);
    statusLedSetBoardRegistered(false);

    Serial.println("\n[Net] Resetting WiFi state...");
    // Turn WiFi off without erasing saved AP configuration from NVS.
    WiFi.disconnect(true, false);
    wifiSetupPhase = WiFiSetupPhase::WaitingToPowerOff;
    wifiSetupPhaseStartedAt = millis();
}

bool authenticate() {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.begin(runtimeApiBaseUrl() + "/login");
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["username"] = runtimeBoardUsername();
    doc["password"] = runtimeBoardPassword();

    String requestBody;
    serializeJson(doc, requestBody);

    int httpCode = http.POST(requestBody);
    bool success = false;

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument responseDoc;
        DeserializationError error = deserializeJson(responseDoc, payload);

        if (!error && responseDoc["token"].is<String>()) {
            jwtToken = responseDoc["token"].as<String>();
            Serial.println("[Net] API Authentication successful. JWT Token acquired.");
            statusLedRecordApiSuccess();
            success = true;
        } else {
            Serial.println("[Net] API Auth response was invalid.");
            statusLedRecordApiFailure(StatusApiError::InvalidResponse);
        }
    } else {
        logHttpFailure("/login",
                       httpCode,
                       (httpCode == 401 || httpCode == 403)
                           ? StatusApiError::Authentication
                           : StatusApiError::None);
    }

    http.end();
    return success;
}

bool registerBoard() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.begin(runtimeApiBaseUrl() + "/register");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    addBoardMetadata(http);

    int httpCode = http.POST("");
    bool success = false;

    if (httpCode == HTTP_CODE_OK) {
        WiFiClient* stream = http.getStreamPtr();
        uint8_t response[2];

        if (readExact(stream, response, sizeof(response)) && response[0] == 1) {
            Serial.println("[Net] Board binary registration successful!");
            boardRegistered = true;
            statusLedSetBoardRegistered(true);
            statusLedRecordApiSuccess();
            success = true;
        } else {
            Serial.println("[Net] Registration response was incomplete or invalid.");
            statusLedRecordApiFailure(StatusApiError::Registration);
        }
    } else {
        logHttpFailure("/register",
                       httpCode,
                       (httpCode >= 0 && httpCode < 500)
                           ? StatusApiError::Registration
                           : StatusApiError::None);
    }

    if (!success) {
        // Preserve the existing behavior of forcing a fresh login after
        // registration failure, including malformed/incomplete 200 responses.
        jwtToken = "";
        boardRegistered = false;
        statusLedSetBoardRegistered(false);
    }

    http.end();
    return success;
}

void pollGameState() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(runtimeApiBaseUrl() + "/poll_binary");
    http.addHeader("Authorization", "Bearer " + jwtToken);

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        if (http.getSize() == 0) {
            // An inactive game is represented by an empty legacy response.
            // Do not enter a blocking stream read or discard the last valid state.
            setBuildingScanScenarioState(false, true);
            statusLedRecordApiSuccess();
            http.end();
            return;
        }

        WiFiClient* stream = http.getStreamPtr();
        uint8_t prodCount = 0;

        bool ok = readByte(stream, prodCount);
        if (ok && prodCount <= 9) {
            for (uint8_t i = 0; i < prodCount && ok; ++i) {
                uint8_t sourceId = 0;
                int32_t coeffMw = 0;
                ok = readByte(stream, sourceId) && readBE32(stream, coeffMw);
                if (ok && sourceId <= 8) {
                    currentCoefficient[sourceId] = coeffMw / 1000.0f;
                }
            }

            uint8_t consCount = 0;
            if (ok) ok = readByte(stream, consCount);
            if (ok && consCount <= BUILDING_COUNT) {
                for (uint8_t i = 0; i < consCount && ok; ++i) {
                    uint8_t buildingId = 0;
                    int32_t consumptionMw = 0;
                    ok = readByte(stream, buildingId) &&
                         readBE32(stream, consumptionMw);
                }
            } else if (ok) {
                Serial.printf("[Net] /poll_binary invalid consumption count: %u\n", consCount);
                ok = false;
            }
        } else if (ok) {
            Serial.printf("[Net] /poll_binary invalid production count: %u\n", prodCount);
            ok = false;
        }

        if (!ok) {
            Serial.println("[Net] /poll_binary body was incomplete or invalid; keeping previous state.");
            statusLedRecordApiFailure(StatusApiError::InvalidResponse);
        } else {
            setBuildingScanScenarioState(true, false);
            statusLedRecordApiSuccess();
        }
    } else {
        if (!handleAuthFailure(httpCode, "/poll_binary") &&
            !handleBoardNotFound(http, httpCode, "/poll_binary")) {
            logHttpFailure("/poll_binary", httpCode);
        }
    }

    http.end();
}

void pollProductionRanges() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(runtimeApiBaseUrl() + "/prod_vals");
    http.addHeader("Authorization", "Bearer " + jwtToken);

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        WiFiClient* stream = http.getStreamPtr();
        uint8_t count = 0;
        bool ok = readByte(stream, count);

        if (ok && count <= 9) {
            for (uint8_t i = 0; i < count && ok; ++i) {
                uint8_t sourceId = 0;
                int32_t minPowerMw = 0;
                int32_t maxPowerMw = 0;

                ok = readByte(stream, sourceId) &&
                     readBE32(stream, minPowerMw) &&
                     readBE32(stream, maxPowerMw);

                if (ok && sourceId <= 8) {
                    baseMinMW[sourceId] = minPowerMw / 1000;
                    baseMaxMW[sourceId] = maxPowerMw / 1000;
                }
            }
        } else if (ok) {
            Serial.printf("[Net] /prod_vals invalid count: %u\n", count);
            ok = false;
        }

        if (!ok) {
            Serial.println("[Net] /prod_vals body was incomplete or invalid.");
            statusLedRecordApiFailure(StatusApiError::InvalidResponse);
        } else {
            statusLedRecordApiSuccess();
        }
    } else {
        if (!handleAuthFailure(httpCode, "/prod_vals") &&
            !handleBoardNotFound(http, httpCode, "/prod_vals")) {
            logHttpFailure("/prod_vals", httpCode);
        }
    }

    http.end();
}

void pollConsumptionValues() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(runtimeApiBaseUrl() + "/cons_vals");
    http.addHeader("Authorization", "Bearer " + jwtToken);

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        WiFiClient* stream = http.getStreamPtr();
        uint8_t count = 0;
        bool ok = readByte(stream, count);

        if (ok && count <= BUILDING_COUNT) {
            for (uint8_t i = 0; i < count && ok; ++i) {
                uint8_t buildingId = 0;
                int32_t consumptionMw = 0;

                ok = readByte(stream, buildingId) &&
                     readBE32(stream, consumptionMw);

                if (ok && buildingId < BUILDING_COUNT) {
                    buildingConsumptionMW[buildingId] = consumptionMw / 1000;
                }
            }
        } else if (ok) {
            Serial.printf("[Net] /cons_vals invalid count: %u\n", count);
            ok = false;
        }

        if (!ok) {
            Serial.println("[Net] /cons_vals body was incomplete or invalid.");
            statusLedRecordApiFailure(StatusApiError::InvalidResponse);
        } else {
            statusLedRecordApiSuccess();
        }
    } else {
        if (!handleAuthFailure(httpCode, "/cons_vals") &&
            !handleBoardNotFound(http, httpCode, "/cons_vals")) {
            logHttpFailure("/cons_vals", httpCode);
        }
    }

    http.end();
}

SyncV2Result syncBoardV2() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED || !boardRegistered) {
        return SyncV2Result::Failed;
    }

    const uint32_t sequence = ++syncSequence;
    uint8_t requestBody[SYNC_V2_REQUEST_SIZE] = {0};
    requestBody[0] = 'E';
    requestBody[1] = 'A';
    requestBody[2] = 2;
    requestBody[3] = 0;
    writeBEU32(requestBody + 4, sequence);
    writeBE32(requestBody + 8, currentTotalProduction_MW);
    writeBE32(requestBody + 12, currentTotalConsumption_MW);
    for (size_t sourceId = 0; sourceId < 9; ++sourceId) {
        writeBE32(requestBody + 16 + sourceId * 4, productionByTypeMW[sourceId]);
    }

    HTTPClient http;
    http.begin(runtimeApiBaseUrl() + "/board/sync/v2");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    addBoardMetadata(http);
    http.addHeader("Content-Type", "application/octet-stream");
    http.addHeader("X-ENAK-Firmware-Protocol", "1");

    int httpCode = http.POST(requestBody, sizeof(requestBody));
    if (httpCode == HTTP_CODE_NOT_FOUND) {
        String body = http.getString();
        body.trim();
        if (body == "BOARD_NOT_FOUND") {
            Serial.println("[Net] /board/sync/v2 requires board re-registration.");
            boardRegistered = false;
            statusLedSetBoardRegistered(false);
            statusLedRecordApiFailure(StatusApiError::Registration);
            http.end();
            return SyncV2Result::Failed;
        }

        Serial.println("[Net] /board/sync/v2 is unavailable; temporarily using legacy endpoints.");
        statusLedRecordApiSuccess();
        http.end();
        return SyncV2Result::NotSupported;
    }

    if (httpCode != HTTP_CODE_OK) {
        if (!handleAuthFailure(httpCode, "/board/sync/v2")) {
            logHttpFailure("/board/sync/v2", httpCode);
        }
        http.end();
        return SyncV2Result::Failed;
    }

    int responseSize = http.getSize();
    if (responseSize >= 0 && responseSize != static_cast<int>(SYNC_V2_RESPONSE_SIZE)) {
        Serial.printf("[Net] /board/sync/v2 invalid response size: %d\n", responseSize);
        statusLedRecordApiFailure(StatusApiError::InvalidResponse);
        http.end();
        return SyncV2Result::Failed;
    }

    uint8_t response[SYNC_V2_RESPONSE_SIZE];
    if (!readExact(http.getStreamPtr(), response, sizeof(response))) {
        statusLedRecordApiFailure(StatusApiError::InvalidResponse);
        http.end();
        return SyncV2Result::Failed;
    }
    http.end();

    uint8_t flags = response[3];
    uint32_t echoedSequence = readBEU32(response + 4);
    if (response[0] != 'E' || response[1] != 'A' || response[2] != 2 ||
        (flags & ~0x03) != 0 || echoedSequence != sequence) {
        Serial.println("[Net] /board/sync/v2 invalid header or sequence.");
        statusLedRecordApiFailure(StatusApiError::InvalidResponse);
        return SyncV2Result::Failed;
    }

    uint32_t configRevision = readBEU32(response + 8);
    size_t offset = 12;
    float nextCoefficients[9];
    int32_t nextMin[9];
    int32_t nextMax[9];
    uint32_t nextConsumption[BUILDING_COUNT];
    uint8_t nextCounts[BUILDING_COUNT];

    for (size_t i = 0; i < 9; ++i, offset += 4) {
        nextCoefficients[i] = readBE32(response + offset) / 1000.0f;
    }
    for (size_t i = 0; i < 9; ++i, offset += 4) {
        nextMin[i] = readBE32(response + offset) / 1000;
    }
    for (size_t i = 0; i < 9; ++i, offset += 4) {
        nextMax[i] = readBE32(response + offset) / 1000;
    }
    for (size_t i = 0; i < BUILDING_COUNT; ++i, offset += 4) {
        int32_t value = readBE32(response + offset);
        if (value < 0) {
            Serial.println("[Net] /board/sync/v2 rejected negative building consumption.");
            statusLedRecordApiFailure(StatusApiError::InvalidResponse);
            return SyncV2Result::Failed;
        }
        nextConsumption[i] = static_cast<uint32_t>(value / 1000);
    }
    memcpy(nextCounts, response + offset, sizeof(nextCounts));

    const bool gameActive = (flags & 0x01) != 0;
    firmwareModeActive = (flags & 0x02) != 0;
    bool allBuildingCountsZero = true;
    for (size_t i = 0; i < BUILDING_COUNT; ++i) {
        if (nextCounts[i] != 0) {
            allBuildingCountsZero = false;
            break;
        }
    }

    // An inactive response is an explicit scenario boundary. The zero-count
    // fallback handles a fast restart where the board misses that response.
    const bool resetScanCache =
        !gameActive ||
        (configRevision != lastConfigRevision && allBuildingCountsZero);
    setBuildingScanScenarioState(gameActive, resetScanCache);

    memcpy(currentCoefficient, nextCoefficients, sizeof(nextCoefficients));
    memcpy(baseMinMW, nextMin, sizeof(nextMin));
    memcpy(baseMaxMW, nextMax, sizeof(nextMax));
    memcpy(buildingConsumptionMW, nextConsumption, sizeof(nextConsumption));
    memcpy(authoritativeBuildingCounts, nextCounts, sizeof(nextCounts));

    if (configRevision != lastConfigRevision) {
        Serial.printf("[Net] Applied sync v2 config revision %u (%s).\n",
                      static_cast<unsigned>(configRevision),
                      (flags & 0x01) ? "active" : "inactive");
        lastConfigRevision = configRevision;
    }
    statusLedRecordApiSuccess();
    return SyncV2Result::Success;
}

void syncFirmware() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED || !boardRegistered || !firmwareModeActive) return;

    JsonDocument requestDoc;
    requestDoc["protocol"] = 1;
    requestDoc["firmware_version"] = FIRMWARE_VERSION;
    requestDoc["config_schema"] = runtimeConfigSchema();
    requestDoc["ota_port"] = runtimeOtaPort();
    requestDoc["state"] = pullOtaState();
    if (pullOtaJobId().length() > 0) requestDoc["job_id"] = pullOtaJobId();
    if (pullOtaError().length() > 0) requestDoc["error"] = pullOtaError();

    String requestBody;
    serializeJson(requestDoc, requestBody);

    HTTPClient http;
    http.begin(runtimeApiBaseUrl() + "/board/firmware/sync");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST(requestBody);
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument responseDoc;
        DeserializationError error = deserializeJson(responseDoc, payload);
        if (!error) {
            if (responseDoc["firmware_mode"].is<bool>() &&
                !responseDoc["firmware_mode"].as<bool>()) {
                firmwareModeActive = false;
            }
            JsonObject command = responseDoc["command"].as<JsonObject>();
            if (!command.isNull()) {
                String jobId = command["job_id"].as<String>();
                String version = command["version"].as<String>();
                String sha256 = command["sha256"].as<String>();
                String path = command["path"].as<String>();
                uint32_t size = command["size"].as<uint32_t>();
                if (!installPullFirmware(runtimeApiBaseUrl(), jwtToken, jobId, version,
                                         size, sha256, path)) {
                    statusLedRecordApiFailure(StatusApiError::InvalidResponse);
                }
            }
            statusLedRecordApiSuccess();
        } else {
            Serial.println("[Net] Firmware sync response was invalid.");
            statusLedRecordApiFailure(StatusApiError::InvalidResponse);
        }
    } else if (!handleAuthFailure(httpCode, "/board/firmware/sync")) {
        logHttpFailure("/board/firmware/sync", httpCode);
    }
    http.end();
}

void postTelemetry() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(runtimeApiBaseUrl() + "/post_vals");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.addHeader("Content-Type", "application/octet-stream");

    uint8_t payload[8];

    // Convert to the unsigned bit representation before shifting so negative
    // values (for example battery charging) are serialized portably.
    writeBE32(payload, currentTotalProduction_MW);
    writeBE32(payload + 4, currentTotalConsumption_MW);

    int httpCode = http.POST(payload, sizeof(payload));
    if (httpCode < 200 || httpCode >= 300) {
        if (!handleAuthFailure(httpCode, "/post_vals") &&
            !handleBoardNotFound(http, httpCode, "/post_vals")) {
            logHttpFailure("/post_vals", httpCode);
        }
    } else {
        statusLedRecordApiSuccess();
    }

    http.end();
}

void networkTaskImpl(void *pvParameters) {
    for (;;) {
        unsigned long now = millis();

        const bool otaActive = isOtaInProgress();
        setOtaNetworkTaskPaused(otaActive);
        if (otaActive) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (debugNetworkMode) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!advanceWiFiSetup(now)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (processCandidateWifi(now)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (WiFi.status() != WL_CONNECTED) {
            statusLedSetWifiConnected(false);
            if (now - lastNetworkRetry >= NETWORK_RETRY_INTERVAL) {
                lastNetworkRetry = now;
                connectWiFi();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        statusLedSetWifiConnected(true);

        if (jwtToken == "") {
            if (now - lastNetworkRetry >= NETWORK_RETRY_INTERVAL) {
                lastNetworkRetry = now;
                Serial.println("[Net] Attempting API Authentication...");
                if (authenticate()) {
                    registerBoard();
                    mqttBootstrapRetryAt = now;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!boardRegistered) {
            if (now - lastNetworkRetry >= NETWORK_RETRY_INTERVAL) {
                lastNetworkRetry = now;
                Serial.println("[Net] Attempting board re-registration...");
                registerBoard();
                mqttBootstrapRetryAt = now;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (processPendingBuildingReset(now)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bootstrapMqtt(now);
        mqttTransportTick(now);

        // esp-mqtt normally reconnects by itself. Rebuild a client whose
        // reconnect loop has remained stuck for a full minute instead of
        // requiring a physical board reboot.
        if (mqttBootstrapAttempted && !mqttTransportConnected()) {
            if (mqttDisconnectedSince == 0) mqttDisconnectedSince = now;
            if (now - mqttDisconnectedSince >= 60000) {
                Serial.println("[MQTT] Reconnect stalled; rebuilding the MQTT client.");
                mqttTransportStop();
                mqttBootstrapAttempted = false;
                mqttBootstrapRetryAt = now + 1000;
                mqttDisconnectedSince = 0;
            }
        } else {
            mqttDisconnectedSince = 0;
        }

        const bool mqttReady = mqttTransportReady();
        if (!mqttReady) {
            // Preserve the pre-MQTT fallback state machine: prefer the compact
            // 500 ms HTTP v2 sync, and use the legacy multi-request protocol
            // only while v2 is known to be unavailable.
            bool useLegacyProtocol = !syncV2Available &&
                                     static_cast<int32_t>(now - syncV2RetryAt) < 0;

            if (!useLegacyProtocol && now - lastSyncMs >= SYNC_INTERVAL) {
                lastSyncMs = now;
                SyncV2Result result = syncBoardV2();
                if (result == SyncV2Result::Success) {
                    syncV2Available = true;
                } else if (result == SyncV2Result::NotSupported) {
                    firmwareModeActive = false;
                    syncV2Available = false;
                    syncV2RetryAt = now + SYNC_V2_RETRY_INTERVAL_MS;
                    useLegacyProtocol = true;
                } else {
                    firmwareModeActive = false;
                }
            }

            if (!useLegacyProtocol && firmwareModeActive &&
                now - lastFirmwareSyncMs >= FIRMWARE_SYNC_INTERVAL) {
                lastFirmwareSyncMs = now;
                syncFirmware();
            }

            if (useLegacyProtocol && now - lastPollMs >= POLL_INTERVAL) {
                lastPollMs = now;
                pollGameState();

                // A previous request may have invalidated the token.
                if (jwtToken != "") pollProductionRanges();
                if (jwtToken != "") pollConsumptionValues();
                if (jwtToken != "") pollBuildingCounts();
            }

            if (useLegacyProtocol && jwtToken != "" && now - lastPostMs >= POST_INTERVAL) {
                lastPostMs = now;
                postTelemetry();
            }

            // Keep all HTTP operations on this task. This avoids concurrent
            // HTTP/JWT access from the Arduino loop task when NFC buildings
            // are queued.
            processPendingBuildingUpload(now);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void applyMqttFirmwareMode(bool active) {
    firmwareModeActive = active;
}

void startNetworkTask() {
    // Network operations may combine HTTPS, JSON parsing, and pull-OTA
    // processing. Keep enough headroom for those nested calls.
    const BaseType_t result = xTaskCreatePinnedToCore(
        networkTaskImpl, "NetworkTask", 16384, NULL, 1, NULL, 0);
    setOtaNetworkTaskRunning(result == pdPASS);
    Serial.println("[Net] Network Task started on Core 0");
}

bool sendAddBuilding(uint8_t type, const String& uid) {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) {
        return false;
    }

    HTTPClient http;
    http.begin(runtimeApiBaseUrl() + "/board/add_building");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.addHeader("Content-Type", "application/octet-stream");

    std::vector<uint8_t> payload;
    payload.reserve(2 + uid.length());
    payload.push_back(type);
    payload.push_back(static_cast<uint8_t>(uid.length()));
    for (size_t i = 0; i < uid.length(); i++) {
        payload.push_back(uid[i]);
    }

    int code = http.POST(payload.data(), payload.size());
    bool success = code >= 200 && code < 300;

    if (!success) {
        if (!handleAuthFailure(code, "/board/add_building") &&
            !handleBoardNotFound(http, code, "/board/add_building")) {
            logHttpFailure("/board/add_building", code);
        }
    } else {
        Serial.printf("[Net] sendAddBuilding succeeded. HTTP code: %d\n", code);
        statusLedRecordApiSuccess();
    }

    http.end();
    return success;
}

bool removeBuildingFromServer(const String& uid) {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED || uid.length() == 0) {
        return false;
    }

    HTTPClient http;
    http.begin(runtimeApiBaseUrl() + "/board/remove_building");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.addHeader("Content-Type", "text/plain");

    const int code = http.POST(uid);
    const bool success = code >= 200 && code < 300;
    if (!success) {
        if (!handleAuthFailure(code, "/board/remove_building") &&
            !handleBoardNotFound(http, code, "/board/remove_building")) {
            logHttpFailure("/board/remove_building", code);
        }
    } else {
        Serial.printf("[Net] removeBuildingFromServer succeeded. HTTP code: %d\n", code);
        statusLedRecordApiSuccess();
    }

    http.end();
    return success;
}

void pollBuildingCounts() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(runtimeApiBaseUrl() + "/board/get_counts");
    http.addHeader("Authorization", "Bearer " + jwtToken);

    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        WiFiClient* stream = http.getStreamPtr();
        uint8_t counts[BUILDING_COUNT];

        if (readExact(stream, counts, sizeof(counts))) {
            memcpy(authoritativeBuildingCounts, counts, sizeof(counts));
            statusLedRecordApiSuccess();
        } else {
            Serial.println("[Net] /board/get_counts body was incomplete.");
            statusLedRecordApiFailure(StatusApiError::InvalidResponse);
        }
    } else {
        if (!handleAuthFailure(code, "/board/get_counts") &&
            !handleBoardNotFound(http, code, "/board/get_counts")) {
            logHttpFailure("/board/get_counts", code);
        }
    }

    http.end();
}
