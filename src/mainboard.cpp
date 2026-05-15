#include <Arduino.h>
#include <cstring>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "PeripheralFactory.h"

// --- Network & API Configuration ---
#ifndef WIFI_SSID
#define WIFI_SSID "YOUR_SSID"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "YOUR_PASSWORD"
#endif
#ifndef API_BASE_URL
#define API_BASE_URL "http://192.168.1.100/coreapi"
#endif
#ifndef BOARD_USERNAME
#define BOARD_USERNAME "board1"
#endif
#ifndef BOARD_PASSWORD
#define BOARD_PASSWORD "board123"
#endif

String jwtToken = "";
unsigned long lastNetworkRetry = 0;
const unsigned long NETWORK_RETRY_INTERVAL = 5000;

// Hardware Pins
#define OUT_LATCH_PIN 10
#define OUT_DATA_PIN  11
#define SHARED_CLOCK_PIN 12
#define IN_DATA_PIN 13
#define IN_LOAD_PIN 14

#define SUB1_RX_PIN 43
#define SUB1_TX_PIN 44
#define SUB2_RX_PIN 17
#define SUB2_TX_PIN 18
#define SUB3_RX_PIN 9
#define SUB3_TX_PIN 8

#define STATUS_LED_PIN 38

// System Constants
#define ENCODER_MIN_VALUE 0
#define ENCODER_MAX_VALUE 100
#define ENCODER_STEP -1
#define BARGRAPH_LED_COUNT 10
#define DISPLAY_DIGIT_COUNT 8
#define DEVICE_COUNT 6
#define INPUT_REGISTER_COUNT 3
#define SUBSTATION_UART_BAUD 9600

PeripheralFactory factory;
ShiftRegisterChain* outChain = nullptr;
InputShiftRegisterChain* inChain = nullptr;

std::vector<ShiftButton*> buttons;
std::vector<ShiftEncoder*> encoders;
std::vector<Bargraph*> bargraphs;

SegmentDisplayPair disp1, disp2, disp3;
SegmentDisplayPair::Half coalDisplay, hydroDisplay, gasDisplay, nuclearDisplay, batteryDisplay, windPvDisplay;

SegmentDisplay* consumptionDisp = nullptr;
SegmentDisplay* productionDisp = nullptr;

HardwareSerial subSerial1(1);
HardwareSerial subSerial2(2);
HardwareSerial subSerial3(0);

const uint8_t indexToType[DEVICE_COUNT] = {4, 6, 2, 1, 3, 5};
int32_t lastSentValues[DEVICE_COUNT] = {-1, -1, -1, -1, -1, -1};

SemaphoreHandle_t hardwareMutex;
TaskHandle_t PeripheralTaskHandle;

uint32_t currentTotalProduction_mW = 0;
uint32_t currentTotalConsumption_mW = 0;

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

void pollGameState() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(String(API_BASE_URL) + "/poll_binary");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    
    int httpCode = http.GET();
    
    if (httpCode == 200) {
        WiFiClient* stream = http.getStreamPtr();
        
        // Only parse if we have data (server might return empty 200 OK if game is paused/ended)
        if (stream->available() >= 1) {
            Serial.println("\n=== [Game State Update] ===");
            
            // 1. Read Production Coefficients
            uint8_t prod_count = stream->read();
            Serial.printf("Production Sources (%d):\n", prod_count);
            for (int i = 0; i < prod_count; i++) {
                uint8_t source_id = stream->read();
                int32_t coeff_mw = readBE32(stream);
                float coeff = coeff_mw / 1000.0;
                Serial.printf("  -> Source ID: %d | Coeff: %.3f\n", source_id, coeff);
            }
            
            // 2. Read Consumption Coefficients (Building baseline consumptions)
            if (stream->available() >= 1) {
                uint8_t cons_count = stream->read();
                Serial.printf("Consumer Types (%d):\n", cons_count);
                for (int i = 0; i < cons_count; i++) {
                    uint8_t building_id = stream->read();
                    int32_t cons_mw = readBE32(stream);
                    float consumption_w = cons_mw / 1000.0;
                    Serial.printf("  -> Building ID: %d | Base Consumption: %.3f MW\n", building_id, consumption_w);
                }
            }
            
            // 3. Read Connected Buildings (For NFC persistence)
            if (stream->available() >= 1) {
                uint8_t buildings_count = stream->read();
                Serial.printf("Connected Buildings on this Board (%d):\n", buildings_count);
                
                for (int i = 0; i < buildings_count; i++) {
                    if (stream->available() >= 1) {
                        uint8_t uid_len = stream->read();
                        
                        // Read the UID string
                        char uid_buf[256]; 
                        memset(uid_buf, 0, sizeof(uid_buf));
                        if (uid_len > 0 && stream->available() >= uid_len) {
                            stream->readBytes((uint8_t*)uid_buf, uid_len);
                        }
                        
                        // Read the building type
                        uint8_t building_type = 0;
                        if (stream->available() >= 1) {
                            building_type = stream->read();
                        }
                        
                        Serial.printf("  -> UID: %s | Type: %d\n", uid_buf, building_type);
                    }
                }
            }
            Serial.println("===========================\n");
        } else {
            // Server returned 200 OK but no data -> Game is inactive/ended
            Serial.println("[Game State] Server returned empty payload. Game might be paused or ended.");
        }
    } else {
         Serial.printf("[Net] Poll failed. HTTP Code: %d\n", httpCode);
    }
    
    http.end();
}

void postTelemetry() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(String(API_BASE_URL) + "/post_vals");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.addHeader("Content-Type", "application/octet-stream");

    // Format: production(4) + consumption(4) + buildings_count(1) = 9 bytes
    uint8_t payload[9];
    
    // Pack Production (Big Endian)
    payload[0] = (currentTotalProduction_mW >> 24) & 0xFF;
    payload[1] = (currentTotalProduction_mW >> 16) & 0xFF;
    payload[2] = (currentTotalProduction_mW >> 8) & 0xFF;
    payload[3] = currentTotalProduction_mW & 0xFF;
    
    // Pack Consumption (Big Endian)
    payload[4] = (currentTotalConsumption_mW >> 24) & 0xFF;
    payload[5] = (currentTotalConsumption_mW >> 16) & 0xFF;
    payload[6] = (currentTotalConsumption_mW >> 8) & 0xFF;
    payload[7] = currentTotalConsumption_mW & 0xFF;
    
    // Pack Buildings Count (0 for now)
    payload[8] = 0;

    int httpCode = http.POST(payload, sizeof(payload));
    
    if (httpCode != 200) {
        Serial.printf("[Net] Telemetry POST failed. Code: %d\n", httpCode);
    }
    
    http.end();
}

void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;
    
    Serial.printf("[Net] Connecting to WiFi: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

bool authenticate() {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    String loginUrl = String(API_BASE_URL) + "/login";
    http.begin(loginUrl);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<200> doc;
    doc["username"] = BOARD_USERNAME;
    doc["password"] = BOARD_PASSWORD;
    
    String requestBody;
    serializeJson(doc, requestBody);

    int httpCode = http.POST(requestBody);
    bool success = false;

    if (httpCode == 200) {
        String payload = http.getString();
        StaticJsonDocument<512> responseDoc;
        DeserializationError error = deserializeJson(responseDoc, payload);
        
        if (!error && responseDoc.containsKey("token")) {
            jwtToken = responseDoc["token"].as<String>();
            Serial.println("[Net] Authentication successful. Token acquired.");
            success = true;
        }
    } else {
        Serial.printf("[Net] Auth failed. HTTP Code: %d\n", httpCode);
    }

    http.end();
    return success;
}

bool registerBoard() {
    if (jwtToken == "" || WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    String regUrl = String(API_BASE_URL) + "/register";
    http.begin(regUrl);
    http.addHeader("Authorization", "Bearer " + jwtToken);
    
    int httpCode = http.POST("");
    bool success = false;

    if (httpCode == 200) {
        // According to binary_protocol.py, response is: success(1) + message_len(1) + message
        WiFiClient* stream = http.getStreamPtr();
        if (stream->available() >= 2) {
            uint8_t status = stream->read();
            uint8_t len = stream->read();
            if (status == 1) {
                Serial.println("[Net] Board binary registration successful.");
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

void networkTask() {
    unsigned long now = millis();

    // 1. Handle Connection & Auth
    if (WiFi.status() != WL_CONNECTED) {
        if (now - lastNetworkRetry >= NETWORK_RETRY_INTERVAL) {
            lastNetworkRetry = now;
            connectWiFi();
        }
        return;
    }

    if (jwtToken == "") {
        if (now - lastNetworkRetry >= NETWORK_RETRY_INTERVAL) {
            lastNetworkRetry = now;
            if (authenticate()) {
                registerBoard();
            }
        }
        return; // Don't try to poll/post without a token
    }

    // 2. Poll Game State from API
    if (now - lastPollMs >= POLL_INTERVAL) {
        lastPollMs = now;
        pollGameState();
    }

    // 3. Post Hardware State to API
    if (now - lastPostMs >= POST_INTERVAL) {
        lastPostMs = now;
        postTelemetry();
    }
}

void setup() {
    Serial.begin(115200);
    delay(100);
    connectWiFi();
}

void loop() {
    networkTask();
    delay(10);
}