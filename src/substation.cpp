#include <Arduino.h>
#include "master.h"

BusMaster bus;
bool uart_sent_this_cycle = false;
bool blink_sent_this_cycle = false;

char cmd_buffer[64];
uint8_t buf_ptr = 0;

const uint32_t BITBUS_DURATION = 700;
const uint32_t UART_DURATION = 500;

enum CycleState { BITBUS_PHASE, UART_PHASE };
CycleState current_state = BITBUS_PHASE;
uint32_t phase_start_ms = 0;

bool comm_led_state = false;
uint32_t comm_led_timer = 0;

void processUartCommand(char* cmd) {
    comm_led_state = !comm_led_state;
    digitalWrite(LED_PIN, comm_led_state ? LOW : HIGH);
    comm_led_timer = millis();

    if (strcmp(cmd, "PING?") == 0) {
        Serial.println("PONG");
    } 
    // "COUNTS?" is kept for backwards compatibility but is no longer strictly needed
    else if (strcmp(cmd, "COUNTS?") == 0) {
        uint8_t counts[7];
        bus.getActiveCountsByType(counts, 7);
        Serial.print("COUNTS");
        for (int i = 0; i < 7; i++) {
            Serial.print(" ");
            Serial.print(counts[i]);
        }
        Serial.println();
    }
    else if (strncmp(cmd, "BLINK TYPE ", 11) == 0) {
        int t;
        if (sscanf(cmd, "BLINK TYPE %d", &t) == 1) {
            bus.sendCommandToType(t, CMD_LED_BLINK, nullptr, 0);
        }
    }
    else if (strncmp(cmd, "RGB ", 4) == 0) {
        int t, r, g, b;
        if (sscanf(cmd, "RGB %d %d %d %d", &t, &r, &g, &b) == 4) {
            uint8_t payload[3] = {(uint8_t)r, (uint8_t)g, (uint8_t)b};
            bus.sendCommandToType(t, CMD_RGB, payload, 3);
        }
    } 
    else if (strncmp(cmd, "MOTOR ", 6) == 0) {
        int t, state;
        if (sscanf(cmd, "MOTOR %d %d", &t, &state) == 2) {
            if (state > 0) {
                bus.sendCommandToType(t, CMD_MOTOR_ON, nullptr, 0);
            } else {
                bus.sendCommandToType(t, CMD_MOTOR_OFF, nullptr, 0);
            }
        }
    }
}

void setup() {
    Serial.begin(9600); 
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    bus.begin();
    phase_start_ms = millis();
}

void loop() {
    uint32_t now = millis();

    // Read incoming UART continuously
    while (Serial.available() > 0) {
        char c = Serial.read();
        
        if (c == '\n' || c == '\r') {
            if (buf_ptr > 0) {
                cmd_buffer[buf_ptr] = '\0';
                processUartCommand(cmd_buffer);
                buf_ptr = 0;
            }
        } else if (buf_ptr < sizeof(cmd_buffer) - 1) {
            cmd_buffer[buf_ptr++] = c;
        }
    }

    if (current_state == BITBUS_PHASE) {
        if (now - comm_led_timer > 50) {
            digitalWrite(LED_PIN, LOW); 
        }

        bus.loop();

        if (!blink_sent_this_cycle && (now - phase_start_ms >= 500)) {
            bus.sendCommandToAll(CMD_LED_BLINK, nullptr, 0);
            blink_sent_this_cycle = true;
        }

        if (now - phase_start_ms >= BITBUS_DURATION) {
            current_state = UART_PHASE;
            phase_start_ms = now;
            uart_sent_this_cycle = false;
        }
    } 
    else if (current_state == UART_PHASE) {
        if (now - comm_led_timer > 50) {
            digitalWrite(LED_PIN, HIGH); 
        }
        
        if (!uart_sent_this_cycle) {
            // PROACTIVELY broadcast the counts instead of waiting to be asked
            uint8_t counts[7];
            bus.getActiveCountsByType(counts, 7);
            
            Serial.print("COUNTS");
            for (int i = 0; i < 7; i++) {
                Serial.print(" ");
                Serial.print(counts[i]);
            }
            Serial.println();
            Serial.println("STATION_ON");
            
            uart_sent_this_cycle = true;
        }

        if (now - phase_start_ms >= UART_DURATION) {
            current_state = BITBUS_PHASE;
            phase_start_ms = now;
            blink_sent_this_cycle = false;
        }
    }
}