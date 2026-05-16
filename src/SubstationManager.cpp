#include "SubstationManager.h"
#include "Config.h"
#include "GameState.h"

HardwareSerial subSerial1(1);
HardwareSerial subSerial2(2);
HardwareSerial subSerial3(0);

const uint8_t hwTypeMap[DEVICE_COUNT] = {4, 6, 2, 1, 3, 5};
int32_t lastSentValues[DEVICE_COUNT] = {-1, -1, -1, -1, -1, -1};

struct Substation {
    HardwareSerial* port;
    String buffer;
    uint32_t lastAlive;
    bool online;
    uint8_t counts[7];
    bool needsUpdate[DEVICE_COUNT];
	uint8_t pendingCommandIndex;
    uint32_t lastCommandSendMs;
    
    void init(HardwareSerial* p, int rx, int tx) {
        port = p;
        port->begin(SUBSTATION_UART_BAUD, SERIAL_8N1, rx, tx);
        buffer = "";
        online = false;
        lastAlive = 0;
        memset(counts, 0, sizeof(counts));
        for(int i=0; i<DEVICE_COUNT; i++) needsUpdate[i] = true;

		pendingCommandIndex = 0;
		lastCommandSendMs = 0;
    }

    void send(const char* cmd) {
        port->println(cmd);
    }
};

Substation subs[3];
uint8_t totalCounts[7] = {0};

void initSubstations() {
    subs[0].init(&subSerial1, SUB1_RX_PIN, SUB1_TX_PIN);
    subs[1].init(&subSerial2, SUB2_RX_PIN, SUB2_TX_PIN);
    subs[2].init(&subSerial3, SUB3_RX_PIN, SUB3_TX_PIN);
}

void sendPendingSubstationCommands() {
    for (int s = 0; s < 3; s++) {
        Substation& sub = subs[s];
        if (!sub.online) continue;

        if (sub.pendingCommandIndex < DEVICE_COUNT) {
            if (millis() - sub.lastCommandSendMs >= 20) {
                int i = sub.pendingCommandIndex++;
                if (sub.needsUpdate[i]) {
                    uint8_t type = hwTypeMap[i];
                    int32_t val = lastSentValues[i];
                    float pct = encoderPercentages[i] * 100.0f;

                    // Clamp percentage to prevent overflow
                    if (pct < 0.0f) pct = 0.0f;
                    if (pct > 100.0f) pct = 100.0f;

                    uint8_t r = 0, g = 0, b = 0;

                    // Smooth gradient: Red -> Yellow -> Green
                    if (pct <= 50.0f) { 
                        // 0% to 50%: Red is solid, Green fades in
                        r = 255; 
                        g = (uint8_t)((pct / 50.0f) * 255.0f); 
                        b = 0;
                    } else { 
                        // 50% to 100%: Green is solid, Red fades out
                        r = (uint8_t)(((100.0f - pct) / 50.0f) * 255.0f); 
                        g = 255; 
                        b = 0;
                    }

                    char cmd[32];
                    snprintf(cmd, sizeof(cmd), "RGB %d %d %d %d", type, r, g, b);
                    sub.send(cmd);
                    snprintf(cmd, sizeof(cmd), "MOTOR %d %d", type, val > 0 ? 1 : 0);
                    sub.send(cmd);
                }
                sub.lastCommandSendMs = millis();
            }
        }
    }
}

void processSubstationLine(int subIndex, String line) {
    line.trim();
    if (line.length() == 0) return;

    Serial.printf("[SUB %d RX]: %s\n", subIndex, line.c_str());

    Substation& sub = subs[subIndex];

    if (line == "STATION_ON") {
		sub.lastAlive = millis();
		sub.online = true;
        // The master no longer needs to ask for COUNTS?, the substation pushes it proactively.
        
		for (int i = 0; i < DEVICE_COUNT; i++) {
			sub.needsUpdate[i] = true;
		}
        sub.pendingCommandIndex = 0;
	}
    else if (line.startsWith("COUNTS ")) {
        int counts[7];
        if (sscanf(line.c_str(), "COUNTS %d %d %d %d %d %d %d",
            &counts[0], &counts[1], &counts[2], &counts[3], &counts[4], &counts[5], &counts[6]) == 7) {
            sub.lastAlive = millis();
            sub.online = true;
            for (int type = 1; type <= 7; type++) {
                int count = counts[type - 1];
                if (count > sub.counts[type - 1]) {
                    for (int i = 0; i < DEVICE_COUNT; i++) {
                        if (hwTypeMap[i] == type) {
                            sub.needsUpdate[i] = true;
                            break;
                        }
                    }
                }
                sub.counts[type - 1] = count;
            }
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
    for (size_t i = 0; i < DEVICE_COUNT; ++i) {
        int32_t val = encoderValuesMW[i];
        if (val != lastSentValues[i]) {
            lastSentValues[i] = val;
            for(int s = 0; s < 3; s++) {
                subs[s].needsUpdate[i] = true;
                subs[s].pendingCommandIndex = 0;
            }
        }
    }
}

void updateTotalCounts() {
	memset(totalCounts, 0, sizeof(totalCounts));
	int onlineSubstations = 0;

	for (int s = 0; s < 3; s++) {
        // Increased timeout to 4000ms to tolerate Wi-Fi/HTTP blocking jitter
		if (millis() - subs[s].lastAlive > 4000) {
			subs[s].online = false;
			memset(subs[s].counts, 0, sizeof(subs[s].counts));
		}
		if (subs[s].online) {
			onlineSubstations++;
			for (int i = 0; i < 7; i++) {
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

    for (int i = 1; i <= 7; i++) {
        if (connectedCount[i] != totalCounts[i - 1]) {
            countsChanged = true;
            connectedCount[i] = totalCounts[i - 1];
        }
    }

    if (connectedCount[8] != connectedCount[6]) {
        connectedCount[8] = connectedCount[6];
        countsChanged = true;
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
            "  Pump/Bat (6 & 8): %d\n"
            "  Coal     (7): %d\n"
            "========================================\n",
            onlineSubstations, connectedCount[1], connectedCount[2], 
            connectedCount[3], connectedCount[4], connectedCount[5], 
            connectedCount[6], connectedCount[7]
        );
        Serial.print(outBuffer);
    }
}