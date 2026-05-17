#include <Arduino.h>
#include "master.h"

BusMaster bus;
bool uart_sent_this_cycle = false;
bool blink_sent_this_cycle = false;

char cmd_buffer[160];
uint8_t buf_ptr = 0;

const uint32_t BITBUS_DURATION = 700;
const uint32_t UART_DURATION = 500;

enum CycleState { BITBUS_PHASE, UART_PHASE };
CycleState current_state = BITBUS_PHASE;
uint32_t phase_start_ms = 0;

bool comm_led_state = false;
uint32_t comm_led_timer = 0;

static uint8_t rgb_payloads[9][3] = {0}; 
static uint8_t motor_payloads[9] = {0};

static uint32_t last_bus_tx_ms = 0;
static uint8_t bitbus_scan_type = 1;
static uint8_t bitbus_scan_cmd = 0; 

void processUartCommand(char* cmd) {
    comm_led_state = !comm_led_state;
    digitalWrite(LED_PIN, comm_led_state ? LOW : HIGH);
    comm_led_timer = millis();

    if (strcmp(cmd, "PING?") == 0) {
        Serial.println("PONG");
    } 
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
        int t = atoi(cmd + 11);
        if (t > 0) {
            bus.sendCommandToType(t, CMD_LED_BLINK, nullptr, 0);
        }
    }
    else if (strncmp(cmd, "ALLRGB ", 7) == 0) {
        char* p = cmd + 7; 
        uint8_t temp_rgb[9][3];
        int token_count = 0;
        
        for (int t = 1; t <= 8; t++) {
            for (int i = 0; i < 3; i++) {
                while (*p == ' ') p++; 
                if (!*p) break;        
                temp_rgb[t][i] = (uint8_t)atoi(p);  
                token_count++;   
                while (*p && *p != ' ') p++; 
            }
        }
        
        // Structural Checksum: Only apply if no numbers were dropped
        if (token_count == 24) {
            memcpy(rgb_payloads, temp_rgb, sizeof(rgb_payloads));
            Serial.println("ACK_RGB");
        } else {
            Serial.println("ERR_RGB_DROP");
        }
    }
    else if (strncmp(cmd, "ALLMOT ", 7) == 0) {
        char* p = cmd + 7; 
        uint8_t temp_mot[9];
        int token_count = 0;
        
        for (int t = 1; t <= 8; t++) {
            while (*p == ' ') p++; 
            if (!*p) break;        
            temp_mot[t] = (uint8_t)atoi(p);   
            token_count++;  
            while (*p && *p != ' ') p++; 
        }
        
        if (token_count == 8) {
            memcpy(motor_payloads, temp_mot, sizeof(motor_payloads));
            Serial.println("ACK_MOT");
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

    // Changed to 'while' to completely drain the hardware buffer every loop cycle
    while (USART1->STATR & USART_STATR_RXNE) {
        char c = USART1->DATAR & 0xFF; 

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

        if (now - last_bus_tx_ms >= 30) {
            if (bitbus_scan_cmd == 0) {
                bus.sendCommandToType(bitbus_scan_type, CMD_RGB, rgb_payloads[bitbus_scan_type], 3);
                bitbus_scan_cmd = 1; 
            } else {
                if (motor_payloads[bitbus_scan_type] > 0) {
                    bus.sendCommandToType(bitbus_scan_type, CMD_MOTOR_ON, nullptr, 0);
                } else {
                    bus.sendCommandToType(bitbus_scan_type, CMD_MOTOR_OFF, nullptr, 0);
                }
                
                bitbus_scan_cmd = 0; 
                bitbus_scan_type++;
                if (bitbus_scan_type > 8) bitbus_scan_type = 1;
            }
            last_bus_tx_ms = now;
        }

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