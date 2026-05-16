#include "NetworkManager.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include "Config.h"
#include "GameState.h"

unsigned long lastNetworkRetry = 0;
const unsigned long NETWORK_RETRY_INTERVAL = 5000;

unsigned long lastPollMs = 0;
const unsigned long POLL_INTERVAL = 2000;
unsigned long lastPostMs = 0;
const unsigned long POST_INTERVAL = 1000;

uint32_t swap_uint32(uint32_t val) {
    return (val << 24) | ((val << 8) & 0x00FF0000) |
           ((val >> 8) & 0x0000FF00) | (val >> 24);
}

int32_t readBE32(WiFiClient* stream) {
    uint8_t buf[4];
    if (stream->readBytes(buf, 4) == 4) {
        return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    }
    return 0;
}

void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.printf("[Net] Reconnecting to WiFi: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void networkSetup() {
    Serial.println("\n[Net] Wiping old WiFi cache...");
    WiFi.disconnect(true, true);
    delay(1000);
    
    Serial.println("[Net] Setting Station Mode...");
    WiFi.mode(WIFI_STA);
    delay(100);
    
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

    if (httpCode == 200) {
        String payload = http.getString();
        JsonDocument responseDoc;
        DeserializationError error = deserializeJson(responseDoc, payload);
        
        if (!error && responseDoc["token"].is<String>()) {
            jwtToken = responseDoc["token"].as<String>();
            Serial.println("[Net] API Authentication successful. JWT Token acquired.");
            success = true;
        }
    } else {
        Serial.printf("[Net] API Auth failed. HTTP Code: %d\n", httpCode);
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

    if (httpCode == 200) {
        WiFiClient* stream = http.getStreamPtr();
        if (stream->available() >= 2) {
            uint8_t status = stream->read();
            stream->read(); // len
            if (status == 1) {
                Serial.println("[Net] Board binary registration successful!");
                success = true;
            }
        }
    } else {
        Serial.printf("[Net] Registration failed. HTTP Code: %d\n", httpCode);
        jwtToken = ""; // Force re-auth on next cycle
    }
    http.end();
    return success;
}

void pollGameState() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(String(API_BASE_URL) + "/poll_binary");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    
    if (http.GET() == 200) {
        WiFiClient* stream = http.getStreamPtr();
        if (stream->available() >= 1) {
            uint8_t prod_count = stream->read();
            for (int i = 0; i < prod_count; i++) {
                uint8_t source_id = stream->read();
                int32_t coeff_mw = readBE32(stream);
                float coeff = coeff_mw / 1000.0;
                
                if (source_id <= 8) currentCoefficient[source_id] = coeff;
            }
            if (stream->available() >= 1) {
                uint8_t cons_count = stream->read();
                for (int i = 0; i < cons_count; i++) {
                    stream->read(); // building_id
                    readBE32(stream); // cons_mw
                }
            }
        } else {
            for(int j = 0; j <= 8; j++) currentCoefficient[j] = 0.0;
        }
    }
    http.end();
}

void pollProductionRanges() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return;
    
    HTTPClient http;
    http.begin(String(API_BASE_URL) + "/prod_vals");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    
    if (http.GET() == 200) {
        WiFiClient* stream = http.getStreamPtr();
        if (stream->available() >= 1) {
            uint8_t count = stream->read();
            for (int i = 0; i < count; i++) {
                uint8_t source_id = stream->read();
                int32_t min_power_mw = readBE32(stream) / 1000;
                int32_t max_power_mw = readBE32(stream) / 1000;
                
                if (source_id <= 8) {
                    baseMinMW[source_id] = min_power_mw;
                    baseMaxMW[source_id] = max_power_mw;
                }
            }
        }
    }
    http.end();
}

void pollConsumptionValues() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return;
    
    HTTPClient http;
    http.begin(String(API_BASE_URL) + "/cons_vals");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    
    if (http.GET() == 200) {
        WiFiClient* stream = http.getStreamPtr();
        if (stream->available() >= 1) {
            uint8_t count = stream->read();
            for (int i = 0; i < count; i++) {
                uint8_t b_id = stream->read();
                int32_t cons_mw = readBE32(stream);
                if (b_id < 0x12) {
                    buildingConsumptionMW[b_id] = cons_mw;
                }
            }
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

    std::vector<uint8_t> payload;
    payload.reserve(32 + scannedBuildings.size() * 12);

    payload.push_back((currentTotalProduction_mW >> 24) & 0xFF);
    payload.push_back((currentTotalProduction_mW >> 16) & 0xFF);
    payload.push_back((currentTotalProduction_mW >> 8) & 0xFF);
    payload.push_back(currentTotalProduction_mW & 0xFF);
    
    payload.push_back((currentTotalConsumption_mW >> 24) & 0xFF);
    payload.push_back((currentTotalConsumption_mW >> 16) & 0xFF);
    payload.push_back((currentTotalConsumption_mW >> 8) & 0xFF);
    payload.push_back(currentTotalConsumption_mW & 0xFF);

    // Append connected buildings count and structures
    payload.push_back(scannedBuildings.size() & 0xFF);
    for (const auto& b : scannedBuildings) {
        payload.push_back(b.uid.length() & 0xFF);
        for (size_t i = 0; i < b.uid.length(); i++) {
            payload.push_back(b.uid[i]);
        }
        payload.push_back(b.type);
    }

    http.POST(payload.data(), payload.size());
    http.end();
}

void networkTaskImpl(void *pvParameters) {
    for (;;) {
        unsigned long now = millis();
        
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

        if (now - lastPollMs >= POLL_INTERVAL) {
            lastPollMs = now;
            pollGameState();
            pollProductionRanges();
            pollConsumptionValues(); // Added consumption tracking poll
        }

        if (now - lastPostMs >= POST_INTERVAL) {
            lastPostMs = now;
            postTelemetry();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void startNetworkTask() {
    xTaskCreatePinnedToCore(networkTaskImpl, "NetworkTask", 8192, NULL, 1, NULL, 0);
    Serial.println("[Net] Network Task started on Core 0");
}