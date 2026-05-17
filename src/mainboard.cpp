#include <Arduino.h>
#include <vector>
#include "Config.h"
#include "GameState.h"
#include "NetworkManager.h"
#include "SubstationManager.h"
#include "PeripheralFactory.h"
#include <PN532_I2C.h>
#include <PN532.h>
#include <algorithm>

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

hw_timer_t *displayTimer = nullptr;
uint32_t lastLedToggleMs = 0;
uint32_t lastCountsUpdateMs = 0;
uint32_t lastDisplaysUpdateMs = 0;
uint32_t lastBargraphsUpdateMs = 0;
bool ledState = false;
LED* statusLed = nullptr;

PN532_I2C pn532_i2c(Wire);
PN532 nfc(pn532_i2c);

void IRAM_ATTR onDisplayTimer() {
    factory.update();
}

void updateDisplays() {
    int32_t combinedBatteryPump = productionByTypeMW[8] + productionByTypeMW[6];
    int32_t combinedWindSolar = productionByTypeMW[2] + productionByTypeMW[1];

    if (currentCoefficient[7] > 0.0) coalDisplay.displayNumber(productionByTypeMW[7], 0);
    else coalDisplay.clear();
    if (currentCoefficient[5] > 0.0) hydroDisplay.displayNumber(productionByTypeMW[5], 0);
    else hydroDisplay.clear();
    if (currentCoefficient[8] > 0.0 || currentCoefficient[6] > 0.0) batteryDisplay.displayNumber(combinedBatteryPump, 0);
    else batteryDisplay.clear();
    if (currentCoefficient[3] > 0.0) nuclearDisplay.displayNumber(productionByTypeMW[3], 0);
    else nuclearDisplay.clear();
    if (currentCoefficient[4] > 0.0) gasDisplay.displayNumber(productionByTypeMW[4], 0);
    else gasDisplay.clear();
    if (currentCoefficient[2] > 0.0 || currentCoefficient[1] > 0.0) windPvDisplay.displayNumber(combinedWindSolar, 0);
    else windPvDisplay.clear();

    if (consumptionDisp) consumptionDisp->displayNumber(currentTotalConsumption_mW / 1000, 0);
    if (productionDisp) productionDisp->displayNumber(currentTotalProduction_mW / 1000, 0);
}

bool updateBargraphs() {
    bool anyChanged = false;

    for (size_t i = 0; i < DEVICE_COUNT; ++i) {
        if (!encoders[i] || !bargraphs[i]) continue;
        
        int32_t activeMin = 0;
        int32_t activeMax = 0;
        float displayCoeff = 0.0;

        switch(i) {
            case 0:
                activeMin = (int32_t)(baseMinMW[7] * connectedCount[7] * currentCoefficient[7]);
                activeMax = (int32_t)(baseMaxMW[7] * connectedCount[7] * currentCoefficient[7]);
                displayCoeff = currentCoefficient[7];
                break;
            case 1:
                activeMin = (int32_t)(baseMinMW[5] * connectedCount[5] * currentCoefficient[5]);
                activeMax = (int32_t)(baseMaxMW[5] * connectedCount[5] * currentCoefficient[5]);
                displayCoeff = currentCoefficient[5];
                break;
            case 2:
                activeMin = (int32_t)(baseMinMW[8] * connectedCount[8] * currentCoefficient[8]) + 
                            (int32_t)(baseMinMW[6] * connectedCount[6] * currentCoefficient[6]);
                activeMax = (int32_t)(baseMaxMW[8] * connectedCount[8] * currentCoefficient[8]) + 
                            (int32_t)(baseMaxMW[6] * connectedCount[6] * currentCoefficient[6]);
                displayCoeff = max(currentCoefficient[8], currentCoefficient[6]); 
                break;
            case 3:
                activeMin = (int32_t)(baseMinMW[3] * connectedCount[3] * currentCoefficient[3]);
                activeMax = (int32_t)(baseMaxMW[3] * connectedCount[3] * currentCoefficient[3]);
                displayCoeff = currentCoefficient[3];
                break;
            case 4:
                activeMin = (int32_t)(baseMinMW[4] * connectedCount[4] * currentCoefficient[4]);
                activeMax = (int32_t)(baseMaxMW[4] * connectedCount[4] * currentCoefficient[4]);
                displayCoeff = currentCoefficient[4];
                break;
            case 5:
                activeMin = (int32_t)(baseMinMW[2] * connectedCount[2] * currentCoefficient[2]) + 
                            (int32_t)(baseMinMW[1] * connectedCount[1] * currentCoefficient[1]);
                activeMax = (int32_t)(baseMaxMW[2] * connectedCount[2] * currentCoefficient[2]) + 
                            (int32_t)(baseMaxMW[1] * connectedCount[1] * currentCoefficient[1]);
                displayCoeff = max(currentCoefficient[2], currentCoefficient[1]); 
                break;
        }

        int32_t val = encoders[i]->get_value();
        float pct = 0.0f; 

        if (displayCoeff <= 0.0 || activeMax == 0) {
            val = 0;
            if(encoders[i]->get_value() != 0) {
                encoders[i]->set_value(0);
                anyChanged = true;
            }
            bargraphs[i]->setValue(0);
        } else {
            if (val < activeMin) {
                val = activeMin;
                encoders[i]->set_value(val);
                anyChanged = true;
            } else if (val > activeMax) {
                val = activeMax;
                encoders[i]->set_value(val);
                anyChanged = true;
            }

            // [FIXED] Calculate the percentage based on the true span between activeMin and activeMax
            if (activeMax > activeMin) {
                pct = (float)(val - activeMin) / (float)(activeMax - activeMin);
            } else {
                pct = 1.0f; // Default if min and max are somehow identical
            }

            uint8_t bgVal = 0;
            if (activeMax > activeMin) {
                bgVal = static_cast<uint8_t>(map(val, activeMin, activeMax, 1, BARGRAPH_LED_COUNT));
            } else {
                bgVal = BARGRAPH_LED_COUNT; 
            }
            bargraphs[i]->setValue(bgVal);
        }
        
        // Export the corrected percentage for the SubstationManager
        encoderPercentages[i] = pct;

        if (encoderValuesMW[i] != val) {
            encoderValuesMW[i] = val;
            anyChanged = true;
        }
    }
    return anyChanged;
}

void nfcTaskImpl(void *pvParameters) {
    for (;;) {
        uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
        uint8_t uidLength;

        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100)) {
            String uidStr = "";
            for (uint8_t i = 0; i < uidLength; i++) {
                uidStr += String(uid[i], HEX);
            }

            bool alreadyScanned = false;
            for (const auto& b : scannedBuildings) {
                if (b.uid == uidStr) {
                    alreadyScanned = true;
                    break;
                }
            }

            if (!alreadyScanned) {
                uint8_t data[16] = {0}; 
                bool readSuccess = false;

                if (uidLength == 7) {
                    if (nfc.mifareultralight_ReadPage(4, data)) {
                        readSuccess = true;
                    }
                } else if (uidLength == 4) {
                    uint8_t keyNDEF[6]      = { 0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7 };
                    uint8_t keyUniversal[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

                    if (nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 4, 0, keyNDEF)) {
                        if (nfc.mifareclassic_ReadDataBlock(4, data)) readSuccess = true;
                    } 
                    else if (nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 4, 0, keyUniversal)) {
                        if (nfc.mifareclassic_ReadDataBlock(4, data)) readSuccess = true;
                    }
                }

                if (readSuccess) {
                    bool formatFound = false;
                    uint8_t buildingType = 0;

                    for (int i = 0; i <= 16 - 5; i++) {
                        if (data[i] == 0xD1 && data[i+1] == 0x01 && data[i+2] == 0x01 && data[i+3] == 0x42) {
                            buildingType = data[i+4];
                            formatFound = true;
                            break;
                        }
                    }

                    if (formatFound) {
                        if (buildingType <= 0x11) {
                            scannedBuildings.push_back({uidStr, buildingType});
                            buildingCounts[buildingType]++;
                            
                            Serial.printf("[NFC] SUCCESS! UID: %s | Type: 0x%02X | Total: %d\n", 
                                uidStr.c_str(), buildingType, buildingCounts[buildingType]);
                            
                            tone(BUZZER_PIN, 2000, 150); 
                        } else {
                            Serial.printf("[NFC] REJECTED. Type out of bounds: 0x%02X\n", buildingType);
                            tone(BUZZER_PIN, 300, 300); 
                        }
                    } else {
                        Serial.println("[NFC] REJECTED. Valid NDEF format not found in memory.");
                        tone(BUZZER_PIN, 300, 300); 
                    }
                } else {
                    Serial.printf("[NFC] REJECTED. Failed to read memory. UID length: %d\n", uidLength);
                    tone(BUZZER_PIN, 100, 500); 
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void startNfcTask() {
    xTaskCreatePinnedToCore(nfcTaskImpl, "NFCTask", 4096, NULL, 1, NULL, 0);
}

void setup() {
    Serial.begin(115200);
    delay(3000); 
    
    Serial.println("\n\n[Main] Booting ESP32-S3...");

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(100000);
    
    nfc.begin();
    uint32_t versiondata = nfc.getFirmwareVersion();
    if (!versiondata) {
        Serial.println("[NFC] PN532 board not found via I2C");
    } else {
        Serial.printf("[NFC] Found chip PN5%02X, Firmware ver. %d.%d\n", 
            (versiondata>>24) & 0xFF, (versiondata>>16) & 0xFF, (versiondata>>8) & 0xFF);
        nfc.SAMConfig();
        startNfcTask();
    }
    
    initSubstations();
    networkSetup();
    startNetworkTask();

    statusLed = factory.createLed(STATUS_LED_PIN);

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

    displayTimer = timerBegin(0, 80, true);
    timerAttachInterrupt(displayTimer, &onDisplayTimer, false);
    timerAlarmWrite(displayTimer, 1000, true); 
    timerAlarmEnable(displayTimer);
}

void loop() {
    const uint32_t now = millis();

    if (now - lastCountsUpdateMs >= 200) {
        lastCountsUpdateMs = now;
        updateTotalCounts();
    }

    if (now - lastDisplaysUpdateMs >= 20) {
        lastDisplaysUpdateMs = now;
        updateDisplays();
    }

    if (now - lastBargraphsUpdateMs >= 50) {
        lastBargraphsUpdateMs = now;
        updateBargraphs();
    }

    pollSubstations();
    queueSubstationUpdates();

    uint32_t totalProdThisCycle = 0;
    
    for (size_t i = 0; i < DEVICE_COUNT; ++i) {
        if (i == 2 || i == 5) continue; 
        uint8_t typeID = apiTypeMap[i];
        productionByTypeMW[typeID] = encoderValuesMW[i];
        totalProdThisCycle += encoderValuesMW[i];
    }
    
    int32_t sharedVal = encoderValuesMW[2];
    productionByTypeMW[8] = sharedVal / 2; 
    productionByTypeMW[6] = sharedVal - productionByTypeMW[8]; 
    totalProdThisCycle += sharedVal; 
    
    int32_t solarActiveMax = (int32_t)(baseMaxMW[1] * connectedCount[1] * currentCoefficient[1]);
    int32_t windActiveMax = (int32_t)(baseMaxMW[2] * connectedCount[2] * currentCoefficient[2]);
    
    productionByTypeMW[1] = solarActiveMax; 
    productionByTypeMW[2] = windActiveMax; 
    
    int32_t combinedWeatherPower = solarActiveMax + windActiveMax;
    totalProdThisCycle += combinedWeatherPower;
    
    encoderValuesMW[5] = combinedWeatherPower;
    if (encoders.size() > 5 && encoders[5]) {
        encoders[5]->set_value(combinedWeatherPower);
    }
    
    currentTotalProduction_mW = totalProdThisCycle * 1000;

    // Calculate total consumption by aggregating scanned building loads
    uint32_t totalConsThisCycle = 0;
    for (int i = 0; i < 0x12; i++) {
        totalConsThisCycle += buildingCounts[i] * buildingConsumptionMW[i];
    }
    currentTotalConsumption_mW = totalConsThisCycle;

    if (now - lastLedToggleMs >= 500) {
        lastLedToggleMs = now;
        ledState = !ledState;
        if (statusLed) statusLed->setState(ledState);
    }
}