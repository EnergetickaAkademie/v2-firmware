#include <Arduino.h>
#include "slave.h"

BusSlave powerplant(TYPE_COAL, 0x11223344);
uint32_t led_turn_off_ms = 0;
bool led_active = false;

void handleCommand(uint8_t cmd, const uint8_t* payload, uint8_t len) {
    if (cmd == CMD_LED_BLINK) {
        digitalWrite(LED_PIN, HIGH);
        led_turn_off_ms = millis() + 100;
        led_active = true;
    }
}

void setup() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
    powerplant.begin();
    powerplant.setCommandCallback(handleCommand);
}

void loop() {
    powerplant.listen();

    if (led_active && millis() >= led_turn_off_ms) {
        digitalWrite(LED_PIN, LOW);
        led_active = false;
    }
}