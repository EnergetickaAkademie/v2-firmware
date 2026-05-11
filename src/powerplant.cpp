#include <Arduino.h>
#include "slave.h"
#include "PeripheralFactory.h"

// Hardware Pin Definitions
#define LED_PIN PC0          // Status LED
#define RGB_PIN PD0          // NeoPixel Data Pin
#define MOTOR_PIN_A PC4      // Motor Driver Input 1
#define MOTOR_PIN_B PC5      // Motor Driver Input 2

#ifndef DEVICE_TYPE
#define DEVICE_TYPE TYPE_UNKNOWN
#endif

#ifndef DEVICE_UID
#define DEVICE_UID 0x00000000
#endif

BusSlave powerplant(DEVICE_TYPE, DEVICE_UID);

uint32_t led_turn_off_ms = 0;
bool led_active = false;

PeripheralFactory factory;
SimpleRGB* myRGB = nullptr;

void handleCommand(uint8_t cmd, const uint8_t* payload, uint8_t len) {
    switch (cmd) {
        case CMD_LED_BLINK:
            digitalWrite(LED_PIN, HIGH);
            led_turn_off_ms = millis() + 100;
            led_active = true;
            break;

        case CMD_RGB:
            if (len >= 3 && myRGB != nullptr) {
                myRGB->setColor(payload[0], payload[1], payload[2]);
            }
            break;

        case CMD_MOTOR_ON:
            // Drive forward (Adjust depending on your specific motor driver)
            digitalWrite(MOTOR_PIN_A, HIGH);
            digitalWrite(MOTOR_PIN_B, LOW);
            break;

        case CMD_MOTOR_OFF:
            // Coast / Stop
            digitalWrite(MOTOR_PIN_A, LOW);
            digitalWrite(MOTOR_PIN_B, LOW);
            break;
    }
}

void setup() {
    // Status LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
    // Motor Pins
    pinMode(MOTOR_PIN_A, OUTPUT);
    pinMode(MOTOR_PIN_B, OUTPUT);
    digitalWrite(MOTOR_PIN_A, LOW);
    digitalWrite(MOTOR_PIN_B, LOW);
    
    // Initialize RGB hardware via the Peripheral Factory
    myRGB = factory.createSimpleRGB(RGB_PIN);
    if (myRGB != nullptr) {
        myRGB->setColor(0, 0, 0); // Turn off initially
    }
    
    // BitBus Initialization
    powerplant.begin();
    powerplant.setCommandCallback(handleCommand);
}

void loop() {
    powerplant.listen();
    
    // Flushes hardware peripheral updates (like WS2812 color changes)
    factory.update(); 

    // Handle non-blocking LED blink duration
    if (led_active && millis() >= led_turn_off_ms) {
        digitalWrite(LED_PIN, LOW);
        led_active = false;
    }
}