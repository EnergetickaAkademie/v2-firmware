#include <Arduino.h>
#include <vector>
#include "Config.h"
#include "GameState.h"
#include "NetworkManager.h"
#include "PeripheralFactory.h"

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

// Hardware mapping for sending RGB commands to substations
const uint8_t hwTypeMap[DEVICE_COUNT] = {4, 6, 2, 1, 3, 5};
int32_t lastSentValues[DEVICE_COUNT] = {-1, -1, -1, -1, -1, -1};

SemaphoreHandle_t hardwareMutex;
TaskHandle_t PeripheralTaskHandle;
uint32_t lastLedToggleMs = 0;
bool ledState = false;
LED* statusLed = nullptr;

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

void peripheralTask(void *pvParameters) {
    for (;;) {
        xSemaphoreTake(hardwareMutex, portMAX_DELAY);
        factory.update();
        xSemaphoreGive(hardwareMutex);
        vTaskDelay(pdMS_TO_TICKS(1)); 
    }
}

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
                uint8_t type = hwTypeMap[i];
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
                        if (hwTypeMap[i] == type) {
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
    
    // Copy UART hardware counts directly to our API ID array (1 to 7)
    for (int i = 1; i <= 7; i++) {
        connectedCount[i] = totalCounts[i - 1];
    }
}

void updateDisplays() {
    int32_t combinedBatteryPump = productionByTypeMW[8] + productionByTypeMW[6];
    int32_t combinedWindSolar = productionByTypeMW[2] + productionByTypeMW[1];

    // COAL (API ID 7)
    if (currentCoefficient[7] > 0.0) coalDisplay.displayNumber(productionByTypeMW[7], 0);
    else coalDisplay.clear(); 

    // HYDRO (API ID 5)
    if (currentCoefficient[5] > 0.0) hydroDisplay.displayNumber(productionByTypeMW[5], 0);
    else hydroDisplay.clear();

    // BATTERY (API ID 8) + PUMP (API ID 6)
    if (currentCoefficient[8] > 0.0 || currentCoefficient[6] > 0.0) batteryDisplay.displayNumber(combinedBatteryPump, 0);
    else batteryDisplay.clear();

    // NUCLEAR (API ID 3)
    if (currentCoefficient[3] > 0.0) nuclearDisplay.displayNumber(productionByTypeMW[3], 0);
    else nuclearDisplay.clear();

    // GAS (API ID 4)
    if (currentCoefficient[4] > 0.0) gasDisplay.displayNumber(productionByTypeMW[4], 0);
    else gasDisplay.clear();

    // WIND (API ID 2) + SOLAR/PV (API ID 1)
    if (currentCoefficient[2] > 0.0 || currentCoefficient[1] > 0.0) windPvDisplay.displayNumber(combinedWindSolar, 0);
    else windPvDisplay.clear();

    if (consumptionDisp) consumptionDisp->displayNumber(currentTotalConsumption_mW / 1000, 0);
    if (productionDisp) productionDisp->displayNumber(currentTotalProduction_mW / 1000, 0);
}

void updateBargraphs() {
    updateTotalCounts();

    for (size_t i = 0; i < DEVICE_COUNT; ++i) {
        if (!encoders[i] || !bargraphs[i]) continue;
        
        int32_t activeMin = 0;
        int32_t activeMax = 0;
        float displayCoeff = 0.0;

        // Calculate active bounds based on the mapping
        switch(i) {
            case 0: // COAL (ID 7)
                activeMin = (int32_t)(baseMinMW[7] * connectedCount[7] * currentCoefficient[7]);
                activeMax = (int32_t)(baseMaxMW[7] * connectedCount[7] * currentCoefficient[7]);
                displayCoeff = currentCoefficient[7];
                break;
            case 1: // HYDRO (ID 5)
                activeMin = (int32_t)(baseMinMW[5] * connectedCount[5] * currentCoefficient[5]);
                activeMax = (int32_t)(baseMaxMW[5] * connectedCount[5] * currentCoefficient[5]);
                displayCoeff = currentCoefficient[5];
                break;
            case 2: // BATTERY (ID 8) + PUMP (ID 6)
                activeMin = (int32_t)(baseMinMW[8] * connectedCount[8] * currentCoefficient[8]) + 
                            (int32_t)(baseMinMW[6] * connectedCount[6] * currentCoefficient[6]);
                activeMax = (int32_t)(baseMaxMW[8] * connectedCount[8] * currentCoefficient[8]) + 
                            (int32_t)(baseMaxMW[6] * connectedCount[6] * currentCoefficient[6]);
                displayCoeff = max(currentCoefficient[8], currentCoefficient[6]); 
                break;
            case 3: // NUCLEAR (ID 3)
                activeMin = (int32_t)(baseMinMW[3] * connectedCount[3] * currentCoefficient[3]);
                activeMax = (int32_t)(baseMaxMW[3] * connectedCount[3] * currentCoefficient[3]);
                displayCoeff = currentCoefficient[3];
                break;
            case 4: // GAS (ID 4)
                activeMin = (int32_t)(baseMinMW[4] * connectedCount[4] * currentCoefficient[4]);
                activeMax = (int32_t)(baseMaxMW[4] * connectedCount[4] * currentCoefficient[4]);
                displayCoeff = currentCoefficient[4];
                break;
            case 5: // WIND (ID 2) + SOLAR/PV (ID 1)
                activeMin = (int32_t)(baseMinMW[2] * connectedCount[2] * currentCoefficient[2]) + 
                            (int32_t)(baseMinMW[1] * connectedCount[1] * currentCoefficient[1]);
                activeMax = (int32_t)(baseMaxMW[2] * connectedCount[2] * currentCoefficient[2]) + 
                            (int32_t)(baseMaxMW[1] * connectedCount[1] * currentCoefficient[1]);
                displayCoeff = max(currentCoefficient[2], currentCoefficient[1]); 
                break;
        }

        int32_t val = encoders[i]->get_value();

        // Enforce Bounds and Draw
        if (displayCoeff <= 0.0 || activeMax == 0) {
            val = 0;
            encoders[i]->set_value(0);
            bargraphs[i]->setValue(0);
        } else {
            if (val < activeMin) {
                val = activeMin;
                encoders[i]->set_value(val);
            } else if (val > activeMax) {
                val = activeMax;
                encoders[i]->set_value(val);
            }

            uint8_t bgVal = 0;
            if (activeMax > activeMin) {
                bgVal = static_cast<uint8_t>(map(val, activeMin, activeMax, 1, BARGRAPH_LED_COUNT));
            } else {
                bgVal = BARGRAPH_LED_COUNT; 
            }
            bargraphs[i]->setValue(bgVal);
        }
        
        encoderValuesMW[i] = val;
    }
}

void setup() {
    Serial.begin(115200);
    delay(3000); 
    
    Serial.println("\n\n[Main] Booting ESP32-S3...");
    networkSetup();

    hardwareMutex = xSemaphoreCreateMutex();
    statusLed = factory.createLed(STATUS_LED_PIN);

    subs[0].init(&subSerial1, SUB1_RX_PIN, SUB1_TX_PIN);
    subs[1].init(&subSerial2, SUB2_RX_PIN, SUB2_TX_PIN);
    subs[2].init(&subSerial3, SUB3_RX_PIN, SUB3_TX_PIN);

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
        encoders.push_back(factory.createShiftEncoder(inChain, encoderRegisterIndex[i], encoderBitPosition[i], -10000, 10000, -1));
        buttons.push_back(factory.createShiftButton(inChain, buttonRegisterIndex[i], buttonBitPosition[i], true));
    }

    productionDisp = factory.createSegmentDisplay(outChain, DISPLAY_DIGIT_COUNT);
    consumptionDisp = factory.createSegmentDisplay(outChain, DISPLAY_DIGIT_COUNT);

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
    batteryDisplay = disp2.left();
    nuclearDisplay = disp2.right();
    gasDisplay = disp3.left();
    windPvDisplay = disp3.right();

    factory.createPeriodic(50, []() {
        updateBargraphs();
        updateDisplays();
    });

    xTaskCreatePinnedToCore(peripheralTask, "PeripheralTask", 4096, NULL, 1, &PeripheralTaskHandle, 0);
}

void loop() {
    networkTask();

    xSemaphoreTake(hardwareMutex, portMAX_DELAY);
    uint32_t totalProdThisCycle = 0;
    
    // 1. Tally up the standard 1-to-1 physical encoders
    for (size_t i = 0; i < DEVICE_COUNT; ++i) {
        // Skip the shared Battery/Pump encoder (Index 2)
        // Skip the virtual Weather encoder (Index 5)
        if (i == 2 || i == 5) continue; 
        
        uint8_t typeID = apiTypeMap[i];
        productionByTypeMW[typeID] = encoderValuesMW[i];
        totalProdThisCycle += encoderValuesMW[i];
    }
    
    // 2. Distribute the shared encoder (Index 2) evenly (50/50)
    int32_t sharedVal = encoderValuesMW[2];
    
    // Divide by 2. Subtracting the half prevents lost MWs on odd numbers.
    productionByTypeMW[8] = sharedVal / 2; // Battery (ID 8)
    productionByTypeMW[6] = sharedVal - productionByTypeMW[8]; // Pumped Hydro (ID 6)
    
    totalProdThisCycle += sharedVal; 
    
    // 3. Tally up the VIRTUAL plants (Weather Dependent, No physical encoder)
    int32_t solarActiveMax = (int32_t)(baseMaxMW[1] * connectedCount[1] * currentCoefficient[1]);
    int32_t windActiveMax = (int32_t)(baseMaxMW[2] * connectedCount[2] * currentCoefficient[2]);
    
    productionByTypeMW[1] = solarActiveMax; 
    productionByTypeMW[2] = windActiveMax; 
    
    int32_t combinedWeatherPower = solarActiveMax + windActiveMax;
    totalProdThisCycle += combinedWeatherPower;

    // Feed the combined weather power into the "virtual" 6th encoder 
    // so the bargraph automatically displays the weather output!
    encoderValuesMW[5] = combinedWeatherPower;
    if (encoders.size() > 5 && encoders[5]) {
        encoders[5]->set_value(combinedWeatherPower);
    }

    xSemaphoreGive(hardwareMutex);

    currentTotalProduction_mW = totalProdThisCycle * 1000;

    const uint32_t now = millis();
    if (now - lastLedToggleMs >= 500) {
        lastLedToggleMs = now;
        ledState = !ledState;
        xSemaphoreTake(hardwareMutex, portMAX_DELAY);
        if (statusLed) statusLed->setState(ledState);
        xSemaphoreGive(hardwareMutex);
    }
    
    delay(10);
}