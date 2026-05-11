#include <Arduino.h>
#include <cstring>
#include "PeripheralFactory.h"

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

HardwareSerial subSerial1(1);
HardwareSerial subSerial2(2);
HardwareSerial subSerial3(0);

// Array map: 0=NPP, 1=GAS, 2=BAT, 3=COAL, 4=WIND, 5=HYDRO
const uint8_t indexToType[DEVICE_COUNT] = {4, 6, 2, 1, 3, 5};
int32_t lastSentValues[DEVICE_COUNT] = {-1, -1, -1, -1, -1, -1};

SemaphoreHandle_t hardwareMutex;
TaskHandle_t PeripheralTaskHandle;

// Substation State Machine Struct
struct Substation {
    HardwareSerial* port;
    String buffer;
    uint32_t lastAlive;
    bool online;
    uint8_t counts[7];
    bool needsUpdate[DEVICE_COUNT];
    
    void init(HardwareSerial* p, int rx, int tx) {
        port = p;
        port->begin(SUBSTATION_UART_BAUD, SERIAL_8N1, rx, tx);
        buffer = "";
        online = false;
        lastAlive = 0;
        memset(counts, 0, sizeof(counts));
        for(int i=0; i<DEVICE_COUNT; i++) needsUpdate[i] = true; 
    }

    void send(const char* cmd) {
        port->println(cmd);
    }
};

Substation subs[3];
uint8_t totalCounts[7] = {0};

uint32_t lastReportMs = 0;
uint32_t lastLedToggleMs = 0;
bool ledState = false;
LED* statusLed = nullptr;

// --- Data Processing & UI ---

void updateTotalCounts() {
    memset(totalCounts, 0, sizeof(totalCounts));
    for (int s = 0; s < 3; s++) {
        if (millis() - subs[s].lastAlive > 3000) {
            subs[s].online = false;
            memset(subs[s].counts, 0, sizeof(subs[s].counts));
        }
        
        if (subs[s].online) {
            for (int i = 0; i < 7; i++) {
                totalCounts[i] += subs[s].counts[i];
            }
        }
    }
}

void updateDisplays() {
    updateTotalCounts();

    coalDisplay.displayNumber(totalCounts[3], 0);
    hydroDisplay.displayNumber(totalCounts[5], 0);
    gasDisplay.displayNumber(totalCounts[1], 0);
    nuclearDisplay.displayNumber(totalCounts[0], 0);
    batteryDisplay.displayNumber(totalCounts[2], 0);
    windPvDisplay.displayNumber(totalCounts[4], 0);
}

void updateBargraphs() {
    const size_t deviceCount = min(encoders.size(), bargraphs.size());
    for (size_t i = 0; i < deviceCount; ++i) {
        int32_t val = encoders[i] ? encoders[i]->get_value() : 0;
        val = constrain(val, ENCODER_MIN_VALUE, ENCODER_MAX_VALUE);
        uint8_t bgVal = static_cast<uint8_t>(map(val, ENCODER_MIN_VALUE, ENCODER_MAX_VALUE, 0, BARGRAPH_LED_COUNT));
        bargraphs[i]->setValue(bgVal);
    }
}

// --- Background Task ---

void peripheralTask(void *pvParameters) {
    for (;;) {
        xSemaphoreTake(hardwareMutex, portMAX_DELAY);
        factory.update();
        xSemaphoreGive(hardwareMutex);
        vTaskDelay(pdMS_TO_TICKS(1)); 
    }
}

// --- Serial Communications Protocol ---

void processSubstationLine(int subIndex, String line) {
    line.trim();
    if (line.length() == 0) return;

    Substation& sub = subs[subIndex];

    if (line == "I'm alive") {
        sub.lastAlive = millis();
        sub.online = true;
        
        vTaskDelay(pdMS_TO_TICKS(5)); 
        sub.send("COUNTS?"); 
        
        vTaskDelay(pdMS_TO_TICKS(100)); 
        
        int sentThisCycle = 0;
        for (int i = 0; i < DEVICE_COUNT; i++) {
            bool requiresUpdate = false;
            
            xSemaphoreTake(hardwareMutex, portMAX_DELAY);
            if (sub.needsUpdate[i]) {
                requiresUpdate = true;
                sub.needsUpdate[i] = false;
            }
            xSemaphoreGive(hardwareMutex);

            if (requiresUpdate) {
                uint8_t type = indexToType[i];
                int32_t val = lastSentValues[i];
                
                uint8_t r = 0, g = 0, b = 0;
                if (val < 20) { r = 255; g = 0; b = 0; }
                else if (val > 50) { r = 0; g = 255; b = 0; }
                else { r = 255; g = 255; b = 0; }

                char cmd[32];
                snprintf(cmd, sizeof(cmd), "RGB %d %d %d %d", type, r, g, b);
                sub.send(cmd);
                
                vTaskDelay(pdMS_TO_TICKS(80)); 

                snprintf(cmd, sizeof(cmd), "MOTOR %d %d", type, val > 0 ? 1 : 0);
                sub.send(cmd);
                
                vTaskDelay(pdMS_TO_TICKS(80));

                sentThisCycle++;
                
                if (sentThisCycle >= 2) break; 
            }
        }
    }
    else if (line.startsWith("Type ")) {
        int type, count;
        if (sscanf(line.c_str(), "Type %d: %d", &type, &count) == 2) {
            if (type >= 1 && type <= 7) {
                xSemaphoreTake(hardwareMutex, portMAX_DELAY);
                
                if (count > sub.counts[type - 1]) {
                    for (int i = 0; i < DEVICE_COUNT; i++) {
                        if (indexToType[i] == type) {
                            sub.needsUpdate[i] = true;
                            break;
                        }
                    }
                }
                
                sub.counts[type - 1] = count;
                xSemaphoreGive(hardwareMutex);
            }
        }
    }
}

// --- Main Architecture ---

void setup() {
    Serial.begin(115200);
    delay(3000); 
    Serial.println("[Main] Booting ESP32-S3...");

    hardwareMutex = xSemaphoreCreateMutex();

    statusLed = factory.createLed(STATUS_LED_PIN);

    subs[0].init(&subSerial1, SUB1_RX_PIN, SUB1_TX_PIN);
    subs[1].init(&subSerial2, SUB2_RX_PIN, SUB2_TX_PIN);
    subs[2].init(&subSerial3, SUB3_RX_PIN, SUB3_TX_PIN);
    Serial.println("[Main] 3 Raw Hardware UARTs initialized");

    outChain = factory.createShiftRegisterChain(OUT_LATCH_PIN, OUT_DATA_PIN, SHARED_CLOCK_PIN);
    inChain = factory.createInputShiftRegisterChain(IN_LOAD_PIN, IN_DATA_PIN, SHARED_CLOCK_PIN, INPUT_REGISTER_COUNT);

    encoders.reserve(DEVICE_COUNT);
    buttons.reserve(DEVICE_COUNT);
    bargraphs.reserve(DEVICE_COUNT);

    const uint8_t encoderRegisterIndex[DEVICE_COUNT] = {0, 0, 1, 1, 2, 2};
    const uint8_t encoderBitPosition[DEVICE_COUNT]   = {0, 3, 0, 3, 0, 3};
    const uint8_t buttonRegisterIndex[DEVICE_COUNT]  = {0, 0, 1, 1, 2, 2};
    const uint8_t buttonBitPosition[DEVICE_COUNT]    = {2, 5, 2, 5, 2, 5};

    for (uint8_t i = 0; i < DEVICE_COUNT; ++i) {
        encoders.push_back(factory.createShiftEncoder(inChain, encoderRegisterIndex[i], encoderBitPosition[i], ENCODER_MIN_VALUE, ENCODER_MAX_VALUE, ENCODER_STEP));
        buttons.push_back(factory.createShiftButton(inChain, buttonRegisterIndex[i], buttonBitPosition[i], true));
    }

    Bargraph* bg5 = factory.createBargraph(outChain, BARGRAPH_LED_COUNT);
    Bargraph* bg4 = factory.createBargraph(outChain, BARGRAPH_LED_COUNT);
    disp3 = factory.createSegmentDisplayPair(outChain, DISPLAY_DIGIT_COUNT);
    
    Bargraph* bg3 = factory.createBargraph(outChain, BARGRAPH_LED_COUNT);
    Bargraph* bg2 = factory.createBargraph(outChain, BARGRAPH_LED_COUNT);
    disp2 = factory.createSegmentDisplayPair(outChain, DISPLAY_DIGIT_COUNT);
    
    Bargraph* bg1 = factory.createBargraph(outChain, BARGRAPH_LED_COUNT);
    Bargraph* bg0 = factory.createBargraph(outChain, BARGRAPH_LED_COUNT);
    disp1 = factory.createSegmentDisplayPair(outChain, DISPLAY_DIGIT_COUNT);

    bargraphs.push_back(bg0);
    bargraphs.push_back(bg1);
    bargraphs.push_back(bg2);
    bargraphs.push_back(bg3);
    bargraphs.push_back(bg4);
    bargraphs.push_back(bg5);

    coalDisplay = disp1.left();
    hydroDisplay = disp1.right();
    gasDisplay = disp2.left();
    nuclearDisplay = disp2.right();
    batteryDisplay = disp3.left();
    windPvDisplay = disp3.right();

    for (ShiftEncoder* encoder : encoders) {
        if (encoder) encoder->set_value(50);
    }

    factory.createPeriodic(20, updateDisplays);
    factory.createPeriodic(50, updateBargraphs);

    xTaskCreatePinnedToCore(
        peripheralTask,   
        "PeripheralTask", 
        4096,             
        NULL,             
        1,                
        &PeripheralTaskHandle, 
        0                 
    );
}

void loop() {
    const uint32_t now = millis();

    if (now - lastLedToggleMs >= 500) {
        lastLedToggleMs = now;
        ledState = !ledState;
        
        xSemaphoreTake(hardwareMutex, portMAX_DELAY);
        if (statusLed) statusLed->setState(ledState);
        xSemaphoreGive(hardwareMutex);
    }

    xSemaphoreTake(hardwareMutex, portMAX_DELAY);
    for (size_t i = 0; i < DEVICE_COUNT; ++i) {
        int32_t val = encoders[i] ? encoders[i]->get_value() : 0;
        val = constrain(val, ENCODER_MIN_VALUE, ENCODER_MAX_VALUE);

        if (val != lastSentValues[i]) {
            lastSentValues[i] = val;
            for(int s = 0; s < 3; s++) {
                subs[s].needsUpdate[i] = true;
            }
        }
    }
    xSemaphoreGive(hardwareMutex);

    for (int s = 0; s < 3; s++) {
        while (subs[s].port->available() > 0) {
            char c = subs[s].port->read();
            if (c == '\n') {
                processSubstationLine(s, subs[s].buffer);
                subs[s].buffer = "";
            } else if (c != '\r') {
                subs[s].buffer += c;
            }
        }
    }

    if (now - lastReportMs >= 2000) {
        lastReportMs = now;
        uint8_t detected = 0;
        
        xSemaphoreTake(hardwareMutex, portMAX_DELAY);
        for (int s = 0; s < 3; s++) {
            if (subs[s].online) detected++;
        }
        
        Serial.printf("[Status] Substations Online: %d/3\n", detected);
        Serial.printf("[Grid Data] NPP=%d GAS=%d BAT=%d COAL=%d WIND=%d HYD=%d PUMP=%d\n",
            totalCounts[0], totalCounts[1], totalCounts[2], totalCounts[3], totalCounts[4], totalCounts[5], totalCounts[6]);
        xSemaphoreGive(hardwareMutex);
    }
}