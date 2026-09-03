#include <Arduino.h>
#include "slave.h"
#include "PeripheralFactory.h"

// Hardware pin mapping from v2_powerplant_2026-08-21.net.
constexpr uint32_t POWERPLANT_STATUS_LED_PIN = PA2;
constexpr uint32_t STATUS_RGB_PIN = PC0;
constexpr uint32_t MOTOR_PIN_A = PC4;
constexpr uint32_t MOTOR_PIN_B = PC3;
constexpr uint32_t SOLAR_RGB_PIN = PD0;
constexpr uint32_t SOLAR_ADC_PIN = PA1;

// The percent byte in CMD_MOTOR_ON is retained for protocol compatibility,
// but it is not used as a speed command. Wind motors receive a short
// full-power kick for reliable starting, then run at a fixed reduced duty to
// limit continuous current and voltage drop in the power distribution path.
constexpr uint8_t MOTOR_FULL_DUTY = 255;
constexpr uint8_t WIND_MOTOR_RUN_DUTY = 100;
constexpr uint32_t WIND_MOTOR_KICK_MS = 300;
constexpr uint8_t NEBULIZER_DUTY = 255;
constexpr uint32_t NEBULIZER_COOLDOWN_MS = 5000;
constexpr uint32_t ACTUATOR_COMMAND_TIMEOUT_MS = 2500;

constexpr uint32_t SOLAR_SAMPLE_INTERVAL_MS = 25;
constexpr uint32_t SOLAR_LED_INTERVAL_MS = 100;
constexpr uint16_t SOLAR_ADC_MAX = 1023;
constexpr uint16_t SOLAR_DARK_ADC = (50UL * SOLAR_ADC_MAX) / 3300UL;
constexpr uint16_t SOLAR_BRIGHT_ADC = (1200UL * SOLAR_ADC_MAX) / 3300UL;
constexpr uint8_t SOLAR_LED_BRIGHTNESS = 64;

#ifndef DEVICE_TYPE
#define DEVICE_TYPE TYPE_UNKNOWN
#endif

#ifndef DEVICE_UID
#define DEVICE_UID 0x00000000
#endif

BusSlave powerplant(DEVICE_TYPE, DEVICE_UID);
PeripheralFactory factory;
SimpleRGB* status_rgb = nullptr;
SimpleRGB* solar_rgb = nullptr;

uint32_t status_led_turn_off_ms = 0;
bool status_led_active = false;

uint8_t requested_actuator_percent = 0;
uint8_t driver_duty = 0;
bool actuator_command_received = false;
uint32_t last_actuator_command_ms = 0;

bool motor_running = false;
bool wind_motor_kicking = false;
uint32_t wind_motor_kick_started_ms = 0;

bool nebulizer_running = false;
bool nebulizer_cooling_down = false;
uint32_t nebulizer_stopped_ms = 0;

uint32_t last_solar_sample_ms = 0;
uint32_t last_solar_led_ms = 0;
int32_t filtered_solar_adc_x8 = 0;
bool solar_filter_initialized = false;
uint8_t last_solar_r = 0;
uint8_t last_solar_g = 0;
bool solar_color_initialized = false;

bool isVariableMotorType() {
	return DEVICE_TYPE == TYPE_WIND || DEVICE_TYPE == TYPE_HYDRO;
}

bool isNebulizerType() {
	return DEVICE_TYPE == TYPE_COAL || DEVICE_TYPE == TYPE_NPP;
}

bool hasActuator() {
	return isVariableMotorType() || isNebulizerType();
}

void setDriverDuty(uint8_t duty) {
	if (driver_duty == duty) return;
	driver_duty = duty;

	// Keep the motor pin under timer control on the CH32 Arduino core. Duty 0
	// and 255 produce constant output levels; intermediate wind-motor values
	// use PWM at the frequency configured in setup().
	analogWrite(MOTOR_PIN_A, duty);
	analogWrite(MOTOR_PIN_B, 0);
}

void updateVariableMotor(uint32_t now) {
	if (requested_actuator_percent == 0) {
		if (motor_running) {
			motor_running = false;
			wind_motor_kicking = false;
			setDriverDuty(0);
		}
		return;
	}

	if (!motor_running) {
		motor_running = true;

		if (DEVICE_TYPE == TYPE_WIND) {
			wind_motor_kicking = true;
			wind_motor_kick_started_ms = now;
			setDriverDuty(MOTOR_FULL_DUTY);
			return;
		}
	}

	if (DEVICE_TYPE == TYPE_WIND) {
		if (wind_motor_kicking) {
			if (now - wind_motor_kick_started_ms < WIND_MOTOR_KICK_MS) return;
			wind_motor_kicking = false;
		}

		setDriverDuty(WIND_MOTOR_RUN_DUTY);
		return;
	}

	setDriverDuty(MOTOR_FULL_DUTY);
}

void updateNebulizer(uint32_t now) {
	if (requested_actuator_percent == 0) {
		if (nebulizer_running) {
			nebulizer_running = false;
			nebulizer_cooling_down = true;
			nebulizer_stopped_ms = now;
			setDriverDuty(0);
		}
		return;
	}

	if (nebulizer_running) return;

	if (nebulizer_cooling_down) {
		if (now - nebulizer_stopped_ms < NEBULIZER_COOLDOWN_MS) return;
		nebulizer_cooling_down = false;
	}

	nebulizer_running = true;
	setDriverDuty(NEBULIZER_DUTY);
}

void updateActuator(uint32_t now) {
	if (!hasActuator()) {
		setDriverDuty(0);
		return;
	}

	if (actuator_command_received &&
		now - last_actuator_command_ms >= ACTUATOR_COMMAND_TIMEOUT_MS) {
		actuator_command_received = false;
		requested_actuator_percent = 0;
	}

	if (isVariableMotorType()) updateVariableMotor(now);
	else updateNebulizer(now);
}

void updateSolarIndicator(uint32_t now) {
	if (solar_rgb == nullptr) return;

	if (now - last_solar_sample_ms >= SOLAR_SAMPLE_INTERVAL_MS) {
		last_solar_sample_ms = now;
		const int32_t sample_x8 = static_cast<int32_t>(analogRead(SOLAR_ADC_PIN)) * 8;
		if (!solar_filter_initialized) {
			filtered_solar_adc_x8 = sample_x8;
			solar_filter_initialized = true;
		} else {
			filtered_solar_adc_x8 += (sample_x8 - filtered_solar_adc_x8) / 8;
		}
	}

	if (!solar_filter_initialized || now - last_solar_led_ms < SOLAR_LED_INTERVAL_MS) return;
	last_solar_led_ms = now;

	const uint16_t filtered_adc = filtered_solar_adc_x8 / 8;
	uint8_t level = 0;
	if (filtered_adc >= SOLAR_BRIGHT_ADC) {
		level = 255;
	} else if (filtered_adc > SOLAR_DARK_ADC) {
		level = static_cast<uint32_t>(filtered_adc - SOLAR_DARK_ADC) * 255 /
			(SOLAR_BRIGHT_ADC - SOLAR_DARK_ADC);
	}

	uint8_t r;
	uint8_t g;
	if (level <= 127) {
		r = SOLAR_LED_BRIGHTNESS;
		g = static_cast<uint16_t>(level) * SOLAR_LED_BRIGHTNESS / 127;
	} else {
		r = static_cast<uint16_t>(255 - level) * SOLAR_LED_BRIGHTNESS / 128;
		g = SOLAR_LED_BRIGHTNESS;
	}

	if (!solar_color_initialized || r != last_solar_r || g != last_solar_g) {
		solar_rgb->setColor(r, g, 0);
		last_solar_r = r;
		last_solar_g = g;
		solar_color_initialized = true;
	}
}

void handleCommand(uint8_t cmd, const uint8_t* payload, uint8_t len) {
	switch (cmd) {
		case CMD_LED_BLINK:
			digitalWrite(POWERPLANT_STATUS_LED_PIN, HIGH);
			status_led_turn_off_ms = millis() + 100;
			status_led_active = true;
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
			break;

		case CMD_MOTOR_OFF:
			if (!hasActuator() || len != 0) break;
			requested_actuator_percent = 0;
			last_actuator_command_ms = millis();
			actuator_command_received = true;
			break;
	}
}

void setup() {
	pinMode(POWERPLANT_STATUS_LED_PIN, OUTPUT);
	digitalWrite(POWERPLANT_STATUS_LED_PIN, LOW);

	analogWriteResolution(8);
	analogWriteFrequency(50);
	pinMode(MOTOR_PIN_A, OUTPUT);
	pinMode(MOTOR_PIN_B, OUTPUT);
	analogWrite(MOTOR_PIN_A, 0);
	analogWrite(MOTOR_PIN_B, 0);

	status_rgb = factory.createSimpleRGB(STATUS_RGB_PIN);
	if (status_rgb != nullptr) status_rgb->setColor(5, 0, 5);

	if (DEVICE_TYPE == TYPE_SOLAR) {
		pinMode(SOLAR_ADC_PIN, INPUT_ANALOG);
		solar_rgb = factory.createSimpleRGB(SOLAR_RGB_PIN);
		if (solar_rgb != nullptr) solar_rgb->setColor(SOLAR_LED_BRIGHTNESS, 0, 0);
	}

	powerplant.begin();
	powerplant.setCommandCallback(handleCommand);
}

void loop() {
	powerplant.listen();

	const uint32_t now = millis();
	updateActuator(now);
	updateSolarIndicator(now);
	factory.update();

	if (status_led_active && static_cast<int32_t>(now - status_led_turn_off_ms) >= 0) {
		digitalWrite(POWERPLANT_STATUS_LED_PIN, LOW);
		status_led_active = false;
	}
}
