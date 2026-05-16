#include "NetworkManager.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
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

void postTelemetry() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return;
    
    HTTPClient http;
    http.begin(String(API_BASE_URL) + "/post_vals");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.addHeader("Content-Type", "application/octet-stream");

    uint8_t payload[9];
    payload[0] = (currentTotalProduction_mW >> 24) & 0xFF;
    payload[1] = (currentTotalProduction_mW >> 16) & 0xFF;
    payload[2] = (currentTotalProduction_mW >> 8) & 0xFF;
    payload[3] = currentTotalProduction_mW & 0xFF;
    payload[4] = (currentTotalConsumption_mW >> 24) & 0xFF;
    payload[5] = (currentTotalConsumption_mW >> 16) & 0xFF;
    payload[6] = (currentTotalConsumption_mW >> 8) & 0xFF;
    payload[7] = currentTotalConsumption_mW & 0xFF;
    payload[8] = 0;

    http.POST(payload, sizeof(payload));
    http.end();
}

// Background FreeRTOS Task for Network Operations
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
        }

        if (now - lastPostMs >= POST_INTERVAL) {
            lastPostMs = now;
            postTelemetry();
        }

        // Yield time back to the OS
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void startNetworkTask() {
    // Pin to Core 0 (Protocol/Network Core) so Core 1 (App Core) handles hardware
    xTaskCreatePinnedToCore(
        networkTaskImpl,   // Task function
        "NetworkTask",     // Task name
        8192,              // Stack size
        NULL,              // Parameters
        1,                 // Priority
        NULL,              // Task handle
        0                  // Pin to Core 0
    );
    Serial.println("[Net] Network Task started on Core 0");
}