#include <Arduino.h>

#define RXD_PIN 17 
#define TXD_PIN 18

String subBuffer = "";
String pendingCmd = "";
uint8_t poll_step = 0;

void sendCommand() {
    if (pendingCmd.length() > 0) {
        Serial.print("[ESP32] -> Sending Manual Command: ");
        Serial.println(pendingCmd);
        Serial1.println(pendingCmd);
        pendingCmd = "";
        return;
    }

    switch(poll_step) {
        case 0:
            Serial.println("[ESP32] -> Auto-Polling: Requesting PING");
            Serial1.println("PING?");
            break;
        case 1:
            Serial.println("[ESP32] -> Auto-Polling: Requesting COUNTS");
            Serial1.println("COUNTS?");
            break;
        case 2:
            Serial.println("[ESP32] -> Auto-Polling: Sending RGB Command");
            Serial1.println("RGB 1 255 0 255");
            break;
        case 3:
            Serial.println("[ESP32] -> Auto-Polling: Sending MOTOR Command");
            Serial1.println("MOTOR 2");
            break;
    }
    poll_step = (poll_step + 1) % 3;
}

void processSubstationResponse(String response) {
    response.trim();
    if (response.length() == 0) return;

    if (response == "I'm alive") {
        Serial.println("\n[ESP32] *** Substation 'I'm alive' Received. Synced! ***");
        delay(10); // Give CH32 a tiny buffer to clear its throat
        sendCommand();
    }
    else if (response == "PONG") {
        Serial.println("[ESP32] <<< CH32 Replied: PONG (Connection Solid)");
    } 
    else if (response.startsWith("Type ") || response.startsWith("RGB -") || response.startsWith("MOTOR -")) {
        Serial.print("[ESP32] <<< CH32 Data: ");
        Serial.println(response);
    }
    else {
        Serial.print("[ESP32] <<< CH32 Unknown/Log: ");
        Serial.println(response);
    }
}

void setup() {
    Serial.begin(115200); 
    
    // Crucial: Wait for the USB Serial monitor to actually connect
    delay(3000); 
    
    Serial.println("\n\n=======================================");
    Serial.println("[ESP32] BOOTING MAINBOARD");
    Serial.println("=======================================");
    
    Serial1.begin(9600, SERIAL_8N1, RXD_PIN, TXD_PIN);
    Serial.println("[ESP32] Hardware UART1 Started on Pins 17(RX) / 18(TX) at 9600 baud");
    Serial.println("[ESP32] Waiting for Substation to announce 'I'm alive'...");
}

void loop() {
    // 1. Listen to CH32
    while (Serial1.available() > 0) {
        char c = Serial1.read();
        if (c == '\n') {
            processSubstationResponse(subBuffer);
            subBuffer = ""; 
        } else if (c != '\r') {
            subBuffer += c;
        }
    }

    // 2. Listen to User Keyboard
    while (Serial.available() > 0) {
        char c = Serial.read();
        if (c == '\n') {
            pendingCmd.trim();
            if (pendingCmd.length() > 0) {
                Serial.print("[ESP32] User Queued Command: ");
                Serial.println(pendingCmd);
            }
        } else if (c != '\r') {
            pendingCmd += c;
        }
    }
}