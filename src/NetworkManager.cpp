#include "NetworkManager.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include "Config.h"
#include "GameState.h"
#include "OtaManager.h"

unsigned long lastNetworkRetry = 0;
const unsigned long NETWORK_RETRY_INTERVAL = 5000;

unsigned long lastPollMs = 0;
const unsigned long POLL_INTERVAL = 2000;
unsigned long lastPostMs = 0;
const unsigned long POST_INTERVAL = 1000;

namespace {
constexpr uint32_t STREAM_READ_TIMEOUT_MS = 3000;
bool boardRegistered = false;

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
    return true;
}

void writeBE32(uint8_t* destination, int32_t value) {
    uint32_t raw = static_cast<uint32_t>(value);
    destination[0] = static_cast<uint8_t>(raw >> 24);
    destination[1] = static_cast<uint8_t>(raw >> 16);
    destination[2] = static_cast<uint8_t>(raw >> 8);
    destination[3] = static_cast<uint8_t>(raw);
}

void logHttpFailure(const char* endpoint, int httpCode) {
    if (httpCode < 0) {
        Serial.printf("[Net] %s failed: %s (%d)\n",
                      endpoint,
                      HTTPClient::errorToString(httpCode).c_str(),
                      httpCode);
    } else {
        Serial.printf("[Net] %s failed: HTTP %d\n", endpoint, httpCode);
    }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("[WiFi] Connected. IP=%s RSSI=%d dBm\n",
                          WiFi.localIP().toString().c_str(),
                          WiFi.RSSI());
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
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
    if (now - lastAttemptMs < 2000) return;

    PendingBuilding pending;
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

    if (xSemaphoreTake(pendingMutex, portMAX_DELAY) == pdTRUE) {
        if (!pendingBuildings.empty() &&
            pendingBuildings.front().uid == pending.uid &&
            pendingBuildings.front().type == pending.type) {
            pendingBuildings.erase(pendingBuildings.begin());
        }
        xSemaphoreGive(pendingMutex);
    }

    Serial.println("[Net] Queued building confirmed by server.");
}
} // namespace

void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.printf("[Net] Reconnecting to WiFi: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void networkSetup() {
    WiFi.onEvent(onWiFiEvent);

    Serial.println("\n[Net] Resetting WiFi state...");
    // Turn WiFi off without erasing saved AP configuration from NVS.
    WiFi.disconnect(true, false);
    delay(1000);

    Serial.println("[Net] Setting WiFi OFF mode");
    WiFi.mode(WIFI_OFF);
    delay(1000);

    Serial.println("[Net] Setting Station Mode...");
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    Serial.printf("[Net] Connecting to WiFi: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.print("[Net] Waiting for connection");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[Net] WiFi Connected successfully!");
        Serial.print("[Net] IP Address: ");
        Serial.println(WiFi.localIP());
        Serial.printf("[Net] RSSI: %d dBm\n", WiFi.RSSI());
    } else {
        Serial.println("\n[Net] Initial connection timed out. Will keep trying in background.");
    }
}

bool authenticate() {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.begin(String(API_BASE_URL) + "/login");
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["username"] = BOARD_USERNAME;
    doc["password"] = BOARD_PASSWORD;

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
            success = true;
        } else {
            Serial.println("[Net] API Auth response was invalid.");
        }
    } else {
        logHttpFailure("/login", httpCode);
    }

    http.end();
    return success;
}

bool registerBoard() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.begin(String(API_BASE_URL) + "/register");
    http.addHeader("Authorization", "Bearer " + jwtToken);

    int httpCode = http.POST("");
    bool success = false;

    if (httpCode == HTTP_CODE_OK) {
        WiFiClient* stream = http.getStreamPtr();
        uint8_t response[2];

        if (readExact(stream, response, sizeof(response)) && response[0] == 1) {
            Serial.println("[Net] Board binary registration successful!");
            boardRegistered = true;
            success = true;
        } else {
            Serial.println("[Net] Registration response was incomplete or invalid.");
        }
    } else {
        logHttpFailure("/register", httpCode);
    }

    if (!success) {
        // Preserve the existing behavior of forcing a fresh login after
        // registration failure, including malformed/incomplete 200 responses.
        jwtToken = "";
        boardRegistered = false;
    }

    http.end();
    return success;
}

void pollGameState() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(String(API_BASE_URL) + "/poll_binary");
    http.addHeader("Authorization", "Bearer " + jwtToken);

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        if (http.getSize() == 0) {
            // An inactive game is represented by an empty legacy response.
            // Do not enter a blocking stream read or discard the last valid state.
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
    http.begin(String(API_BASE_URL) + "/prod_vals");
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
    http.begin(String(API_BASE_URL) + "/cons_vals");
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
        }
    } else {
        if (!handleAuthFailure(httpCode, "/cons_vals") &&
            !handleBoardNotFound(http, httpCode, "/cons_vals")) {
            logHttpFailure("/cons_vals", httpCode);
        }
    }

    http.end();
}

void postTelemetry() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(String(API_BASE_URL) + "/post_vals");
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
    }

    http.end();
}

void networkTaskImpl(void *pvParameters) {
    for (;;) {
        unsigned long now = millis();

        if (isOtaInProgress()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (WiFi.status() != WL_CONNECTED) {
            if (now - lastNetworkRetry >= NETWORK_RETRY_INTERVAL) {
                lastNetworkRetry = now;
                connectWiFi();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (jwtToken == "") {
            if (now - lastNetworkRetry >= NETWORK_RETRY_INTERVAL) {
                lastNetworkRetry = now;
                Serial.println("[Net] Attempting API Authentication...");
                if (authenticate()) {
                    registerBoard();
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
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (now - lastPollMs >= POLL_INTERVAL) {
            lastPollMs = now;
            pollGameState();

            // A previous request may have invalidated the token.
            if (jwtToken != "") pollProductionRanges();
            if (jwtToken != "") pollConsumptionValues();
            if (jwtToken != "") pollBuildingCounts();
        }

        if (jwtToken != "" && now - lastPostMs >= POST_INTERVAL) {
            lastPostMs = now;
            postTelemetry();
        }

        // Keep all HTTP operations on this task. This avoids concurrent HTTP/JWT
        // access from the Arduino loop task when NFC buildings are queued.
        processPendingBuildingUpload(now);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void startNetworkTask() {
    xTaskCreatePinnedToCore(networkTaskImpl, "NetworkTask", 8192, NULL, 1, NULL, 0);
    Serial.println("[Net] Network Task started on Core 0");
}

bool sendAddBuilding(uint8_t type, const String& uid) {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) {
        return false;
    }

    HTTPClient http;
    http.begin(String(API_BASE_URL) + "/board/add_building");
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
    }

    http.end();
    return success;
}

void pollBuildingCounts() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(String(API_BASE_URL) + "/board/get_counts");
    http.addHeader("Authorization", "Bearer " + jwtToken);

    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        WiFiClient* stream = http.getStreamPtr();
        uint8_t counts[BUILDING_COUNT];

        if (readExact(stream, counts, sizeof(counts))) {
            memcpy(authoritativeBuildingCounts, counts, sizeof(counts));
        } else {
            Serial.println("[Net] /board/get_counts body was incomplete.");
        }
    } else {
        if (!handleAuthFailure(code, "/board/get_counts") &&
            !handleBoardNotFound(http, code, "/board/get_counts")) {
            logHttpFailure("/board/get_counts", code);
        }
    }

    http.end();
}
