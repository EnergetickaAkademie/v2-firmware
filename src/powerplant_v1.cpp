#include <Arduino.h>
#include <ESP8266WiFi.h>

// Legacy Wemos D1 mini StarWire connector. PodSync keeps the CH32 defaults
// unless these pins are selected before its headers are included.
#define PODSYNC_CLK_PIN D7
#define PODSYNC_DAT_PIN D1
#define PODSYNC_LED_PIN D4
#include "slave.h"

#include "PeripheralFactory.h"

#ifndef DEVICE_TYPE
#define DEVICE_TYPE TYPE_UNKNOWN
#endif

constexpr uint8_t STATUS_LED_PIN = D4;
constexpr uint8_t PERIPHERAL_PIN = D2;
constexpr uint8_t MOTOR_PIN_A = D5;
constexpr uint8_t MOTOR_PIN_B = D6;

constexpr uint16_t MOTOR_FULL_DUTY = 1023;
constexpr uint16_t WIND_MOTOR_RUN_DUTY =
	(200UL * MOTOR_FULL_DUTY + 127UL) / 255UL;
constexpr uint32_t WIND_MOTOR_KICK_MS = 300;
constexpr uint32_t ACTUATOR_COMMAND_TIMEOUT_MS = 10000;
constexpr uint32_t STATUS_BLINK_MS = 100;

BusSlave* powerplant = nullptr;
PeripheralFactory factory;
RGBLED* status_rgb = nullptr;
Motor* motor = nullptr;
Atomizer* atomizer = nullptr;

bool status_led_active = false;
uint32_t status_led_started_ms = 0;

uint8_t requested_actuator_percent = 0;
bool actuator_command_received = false;
uint32_t last_actuator_command_ms = 0;

bool motor_running = false;
bool wind_motor_kicking = false;
uint32_t wind_motor_kick_started_ms = 0;
uint16_t motor_duty = 0;

bool desired_atomizer_on = false;

bool hasRgbPeripheral() {
	return DEVICE_TYPE == TYPE_SOLAR ||
		DEVICE_TYPE == TYPE_GAS ||
		DEVICE_TYPE == TYPE_HYDRO_PUMPED ||
		DEVICE_TYPE == TYPE_BATTERY;
}

bool isVariableMotorType() {
	return DEVICE_TYPE == TYPE_WIND || DEVICE_TYPE == TYPE_HYDRO;
}

bool isAtomizerType() {
	return DEVICE_TYPE == TYPE_NPP || DEVICE_TYPE == TYPE_COAL;
}

bool hasActuator() {
	return isVariableMotorType() || isAtomizerType();
}

void setStatusLed(bool active) {
	// The Wemos D1 mini onboard LED is active-low.
	digitalWrite(STATUS_LED_PIN, active ? LOW : HIGH);
}

void setMotorDuty(uint16_t duty) {
	if (motor == nullptr || duty == motor_duty) return;

	motor_duty = duty;
	if (duty == 0) {
		motor->stop();
	} else {
		motor->forward(duty);
	}
}

void updateVariableMotor(uint32_t now) {
	if (motor == nullptr) return;

	if (actuator_command_received &&
		now - last_actuator_command_ms >= ACTUATOR_COMMAND_TIMEOUT_MS) {
		actuator_command_received = false;
		requested_actuator_percent = 0;
	}

	if (requested_actuator_percent == 0) {
		motor_running = false;
		wind_motor_kicking = false;
		setMotorDuty(0);
		return;
	}

	if (!motor_running) {
		motor_running = true;
		if (DEVICE_TYPE == TYPE_WIND) {
			wind_motor_kicking = true;
			wind_motor_kick_started_ms = now;
			setMotorDuty(MOTOR_FULL_DUTY);
			return;
		}
	}

	if (DEVICE_TYPE == TYPE_WIND) {
		if (wind_motor_kicking &&
			now - wind_motor_kick_started_ms < WIND_MOTOR_KICK_MS) {
			return;
		}

		wind_motor_kicking = false;
		setMotorDuty(WIND_MOTOR_RUN_DUTY);
		return;
	}

	setMotorDuty(MOTOR_FULL_DUTY);
}

void updateAtomizer() {
	if (atomizer == nullptr || atomizer->isActive()) return;

	if (atomizer->getTargetState() != desired_atomizer_on) {
		atomizer->toggle();
	}
}

void handleCommand(uint8_t cmd, const uint8_t* payload, uint8_t len) {
	switch (cmd) {
		case CMD_LED_BLINK:
			if (len != 0) break;
			setStatusLed(true);
			status_led_active = true;
			status_led_started_ms = millis();
			break;

		case CMD_RGB:
			if (len == 3 && payload != nullptr && status_rgb != nullptr) {
				status_rgb->setColor(payload[0], payload[1], payload[2]);
			}
			break;

		case CMD_MOTOR_ON:
			if (!hasActuator() || len != 1 || payload == nullptr ||
				payload[0] == 0 || payload[0] > 100) {
				break;
			}

			requested_actuator_percent = payload[0];
			last_actuator_command_ms = millis();
			actuator_command_received = true;
			if (isAtomizerType()) desired_atomizer_on = true;
			break;

		case CMD_MOTOR_OFF:
			if (!hasActuator() || len != 0) break;

			requested_actuator_percent = 0;
			last_actuator_command_ms = millis();
			actuator_command_received = true;
			if (isAtomizerType()) desired_atomizer_on = false;
			break;
	}
}

void disableWifiRadio() {
	WiFi.persistent(false);
	WiFi.mode(WIFI_OFF);
	WiFi.forceSleepBegin();
	delay(1);
}

void setup() {
	Serial.begin(115200);
	disableWifiRadio();

	pinMode(STATUS_LED_PIN, OUTPUT);
	setStatusLed(false);

	if (hasRgbPeripheral()) {
		status_rgb = factory.createRGBLED(PERIPHERAL_PIN, 1);
		if (status_rgb != nullptr) {
			status_rgb->setBrightness(255);
			status_rgb->setColor(5, 0, 5);
		}
	} else if (isVariableMotorType()) {
		const int pwm_frequency = DEVICE_TYPE == TYPE_WIND ? 20000 : 1000;
		motor = factory.createMotor(MOTOR_PIN_A, MOTOR_PIN_B, pwm_frequency);
		if (motor != nullptr) motor->stop();
	} else if (isAtomizerType()) {
		atomizer = factory.createAtomizer(PERIPHERAL_PIN);
	}

	factory.update();

	static BusSlave bus(DEVICE_TYPE, ESP.getChipId());
	powerplant = &bus;
	powerplant->begin();
	powerplant->setCommandCallback(handleCommand);

	Serial.printf(
		"PodSync v1 powerplant ready: UID=0x%08lX, type=%u\n",
		static_cast<unsigned long>(ESP.getChipId()),
		static_cast<unsigned int>(DEVICE_TYPE)
	);
}

void loop() {
	if (powerplant != nullptr) powerplant->listen();

	const uint32_t now = millis();
	if (isVariableMotorType()) updateVariableMotor(now);
	if (isAtomizerType()) updateAtomizer();

	factory.update();

	if (status_led_active &&
		now - status_led_started_ms >= STATUS_BLINK_MS) {
		setStatusLed(false);
		status_led_active = false;
	}
}
