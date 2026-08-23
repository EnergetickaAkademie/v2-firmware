#include "SubstationManager.h"
#include "DebugLog.h"
#include "Config.h"
#include "GameState.h"
#include "StatusLedManager.h"

#define Serial DebugLog

// USB CDC owns the diagnostic console, leaving all hardware UARTs available.
// Keep SUB1 on UART0's native GPIO44/GPIO43 pair to avoid competing pin-matrix
// ownership on the ESP32-S3 default console pins.
HardwareSerial subSerial1(0);
HardwareSerial subSerial2(2);
HardwareSerial subSerial3(1);

int32_t lastSentValues[DEVICE_COUNT] = {-1, -1, -1, -1, -1, -1};

struct Substation {
    HardwareSerial* port;
    String buffer;
    uint32_t lastAlive;
    bool online;
    uint8_t counts[8];
    bool needsBulkUpdate;
    uint32_t lastCommandSendMs;
    
    void init(HardwareSerial* p, int rx, int tx) {
        port = p;
        port->begin(SUBSTATION_UART_BAUD, SERIAL_8N1, rx, tx);
        buffer = "";
        online = false;
        lastAlive = 0;
        memset(counts, 0, sizeof(counts));
        needsBulkUpdate = true;
        lastCommandSendMs = 0;
    }

    void send(const char* cmd) {
        port->println(cmd);
    }
};

Substation subs[3];
uint8_t totalCounts[8] = {0};
uint8_t onlineSubstationCount = 0;

void initSubstations() {
    subs[0].init(&subSerial1, SUB1_RX_PIN, SUB1_TX_PIN);
    subs[1].init(&subSerial2, SUB2_RX_PIN, SUB2_TX_PIN);
    subs[2].init(&subSerial3, SUB3_RX_PIN, SUB3_TX_PIN);
}

void sendPendingSubstationCommands() {
    for (int s = 0; s < 3; s++) {
        Substation& sub = subs[s];
        if (!sub.online) continue;

        if (sub.needsBulkUpdate && (millis() - sub.lastCommandSendMs >= 150)) {
            // Map encoder percentages to powerplant types 1-8
            int pctMap[8] = {
                (int)(encoderPercentages[5] * 100.0f), // Type 1 (Solar)
                (int)(encoderPercentages[5] * 100.0f), // Type 2 (Wind)
                (int)(encoderPercentages[3] * 100.0f), // Type 3 (Nuclear)
                (int)(encoderPercentages[4] * 100.0f), // Type 4 (Gas)
                (int)(encoderPercentages[1] * 100.0f), // Type 5 (Hydro)
                (int)(encoderPercentages[2] * 100.0f), // Type 6 (PumpStor)
                (int)(encoderPercentages[0] * 100.0f), // Type 7 (Coal)
                (int)(encoderPercentages[2] * 100.0f)  // Type 8 (Battery)
            };

            uint8_t actuatorPct[8] = {
                0,                                             // Type 1 (Solar)
                (uint8_t)constrain(pctMap[1], 0, 100),         // Type 2 (Wind)
                (uint8_t)(encoderValuesMW[3] > 0 ? 100 : 0),   // Type 3 (Nuclear)
                0,                                             // Type 4 (Gas)
                (uint8_t)constrain(pctMap[4], 0, 100),         // Type 5 (Hydro)
                0,                                             // Type 6 (PumpStor)
                (uint8_t)(encoderValuesMW[0] > 0 ? 100 : 0),   // Type 7 (Coal)
                0                                              // Type 8 (Battery)
            };

            String rgbCmd = "ALLRGB";
            String motCmd = "ALLMOT";

            for (int i = 0; i < 8; i++) {
                int pct = pctMap[i];
                pct = constrain(pct, 0, 100);

                uint8_t r = 0, g = 0, b = 0;
                
                // 3-Phase Pronounced Gradient
                if (pct <= 33) {
                    // 0% to 33%: Dark Red fading up to Bright Red
                    r = map(pct, 0, 33, 30, 255); // Starts at 30 (very dim red)
                    g = 0;
                    b = 0;
                } else if (pct <= 66) {
                    // 34% to 66%: Bright Red fading into Orange and then Yellow
                    r = 255;
                    g = map(pct, 34, 66, 0, 255);
                    b = 0;
                } else {
                    // 67% to 100%: Yellow fading into Bright Green
                    r = map(pct, 67, 100, 255, 0);
                    g = 255;
                    b = 0;
                }

                char buf[16];
                snprintf(buf, sizeof(buf), " %d %d %d", r, g, b);
                rgbCmd += buf;

                snprintf(buf, sizeof(buf), " %u", static_cast<unsigned>(actuatorPct[i]));
                motCmd += buf;
            }

            sub.send(rgbCmd.c_str());
            sub.send(motCmd.c_str());

            sub.needsBulkUpdate = false;
            sub.lastCommandSendMs = millis();
        }
    }
}

void processSubstationLine(int subIndex, String line) {
    line.trim();
    if (line.length() == 0) return;

    Serial.printf("[SUB %d RX]: %s\n", subIndex, line.c_str());

    Substation& sub = subs[subIndex];

    if (line == "STATION_ON") {
        const bool wasOnline = sub.online;
        sub.lastAlive = millis();
        sub.online = true;
        if (!wasOnline) sub.needsBulkUpdate = true;
    }
    else if (line.startsWith("COUNTS ")) {
        int counts[8];
        char trailing;
        const int parsed = sscanf(line.c_str(),
            "COUNTS %d %d %d %d %d %d %d %d %c",
            &counts[0], &counts[1], &counts[2], &counts[3],
            &counts[4], &counts[5], &counts[6], &counts[7], &trailing);

        bool valid = parsed == 8;
        for (int i = 0; valid && i < 8; i++) {
            valid = counts[i] >= 0 && counts[i] <= 255;
        }

        if (valid) {
            const bool wasOnline = sub.online;
            bool countsChanged = false;
            sub.lastAlive = millis();
            sub.online = true;
            for (int i = 0; i < 8; i++) {
                if (sub.counts[i] != counts[i]) {
                    countsChanged = true;
                    sub.counts[i] = counts[i];
                }
            }
            if (!wasOnline || countsChanged) sub.needsBulkUpdate = true;
        }
    }
}

void pollSubstations() {
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
    sendPendingSubstationCommands();
}

void queueSubstationUpdates() {
    static uint32_t lastUpdateCheckMs = 0;
    
    // Only check for encoder changes every 500ms (adjust this value to your liking)
    if (millis() - lastUpdateCheckMs < 500) {
        return; 
    }
    lastUpdateCheckMs = millis();

    for (size_t i = 0; i < DEVICE_COUNT; ++i) {
        int32_t val = encoderValuesMW[i];
        if (val != lastSentValues[i]) {
            lastSentValues[i] = val;
            for(int s = 0; s < 3; s++) {
                subs[s].needsBulkUpdate = true;
            }
        }
    }
}

void updateTotalCounts() {
    memset(totalCounts, 0, sizeof(totalCounts));
    uint8_t onlineSubstations = 0;

    for (int s = 0; s < 3; s++) {
        if (millis() - subs[s].lastAlive > 4000) {
            subs[s].online = false;
            memset(subs[s].counts, 0, sizeof(subs[s].counts));
        }
        if (subs[s].online) {
            onlineSubstations++;
            for (int i = 0; i < 8; i++) {
                totalCounts[i] += subs[s].counts[i];
            }
        }
    }
    
    bool countsChanged = false;
    static int lastOnlineSubstations = -1;
    
    if (onlineSubstations != lastOnlineSubstations) {
        countsChanged = true;
        lastOnlineSubstations = onlineSubstations;
    }
    onlineSubstationCount = onlineSubstations;
    statusLedSetSubstationCount(onlineSubstationCount);

    for (int i = 1; i <= 8; i++) {
        if (connectedCount[i] != totalCounts[i - 1]) {
            countsChanged = true;
            connectedCount[i] = totalCounts[i - 1];
        }
    }

    static uint32_t lastPrintMs = 0;
    bool forcePrint = false;
    if (millis() - lastPrintMs >= 3000) {
        forcePrint = true;
        lastPrintMs = millis();
    }

    if (countsChanged || forcePrint) {
        char outBuffer[512];
        snprintf(outBuffer, sizeof(outBuffer),
            "\n=== [Physical Power Plants Detected] ===\n"
            "  Connected Substations: %d/3\n"
            "  Solar/PV (1): %d\n"
            "  Wind     (2): %d\n"
            "  Nuclear  (3): %d\n"
            "  Gas      (4): %d\n"
            "  Hydro    (5): %d\n"
            "  Pumped   (6): %d\n"
            "  Coal     (7): %d\n"
            "  Battery  (8): %d\n"
            "========================================\n",
            onlineSubstations, connectedCount[1], connectedCount[2], 
            connectedCount[3], connectedCount[4], connectedCount[5], 
            connectedCount[6], connectedCount[7], connectedCount[8]
        );

        //Serial.print(outBuffer);
    }
}

uint8_t getOnlineSubstationCount() {
    return onlineSubstationCount;
}
