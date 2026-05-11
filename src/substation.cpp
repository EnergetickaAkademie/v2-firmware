#include <Arduino.h>
#include "master.h"

BusMaster bus;
bool uart_sent_this_cycle = false;
bool blink_sent_this_cycle = false;

char cmd_buffer[32];
uint8_t buf_ptr = 0;

const uint32_t BITBUS_DURATION = 700;
const uint32_t UART_DURATION = 500;

enum CycleState { BITBUS_PHASE, UART_PHASE };
CycleState current_state = BITBUS_PHASE;
uint32_t phase_start_ms = 0;

void processUartCommand(char* cmd) {
	if (strcmp(cmd, "PING?") == 0) {
		Serial.println("PONG");
	} 
	
	else if (strcmp(cmd, "COUNTS?") == 0) {
		uint8_t counts[7];
		bus.getActiveCountsByType(counts, 7);
		for (int i = 0; i < 7; i++) {
			Serial.print("Type "); Serial.print(i + 1);
			Serial.print(": "); Serial.println(counts[i]);
		}
	}
	
	else if (strncmp(cmd, "BLINK TYPE ", 11) == 0) {
		int t;
		if (sscanf(cmd, "BLINK TYPE %d", &t) == 1) {
			bus.sendCommandToType(t, CMD_LED_BLINK, nullptr, 0);
			Serial.print("BLINK - Type: "); Serial.println(t);
		}
	}
	
	else if (strncmp(cmd, "RGB ", 4) == 0) {
		int t, r, g, b;
		if (sscanf(cmd, "RGB %d %d %d %d", &t, &r, &g, &b) == 4) {
			// Forward the RGB data down the BitBus
			uint8_t payload[3] = {(uint8_t)r, (uint8_t)g, (uint8_t)b};
			bus.sendCommandToType(t, CMD_RGB, payload, 3);

			Serial.print("RGB - Type: "); Serial.print(t);
			Serial.print(" R:"); Serial.print(r);
			Serial.print(" G:"); Serial.print(g);
			Serial.print(" B:"); Serial.println(b);
		}
	} 
	
	else if (strncmp(cmd, "MOTOR ", 6) == 0) {
		int t, state;
		// Parse the Type and the State (1 = ON, 0 = OFF) sent by the Mainboard
		if (sscanf(cmd, "MOTOR %d %d", &t, &state) == 2) {
			
			// Forward the appropriate motor command down the BitBus
			if (state > 0) {
				bus.sendCommandToType(t, CMD_MOTOR_ON, nullptr, 0);
			} else {
				bus.sendCommandToType(t, CMD_MOTOR_OFF, nullptr, 0);
			}

			Serial.print("MOTOR - Type: "); Serial.print(t);
			Serial.print(" State: "); Serial.println(state);
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

	if (current_state == BITBUS_PHASE) {
		digitalWrite(LED_PIN, LOW); 
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
		digitalWrite(LED_PIN, HIGH); 
		
		if (!uart_sent_this_cycle) {
			// BYPASS ARDUINO CORE: Flush hardware register directly
			while (USART1->STATR & USART_STATR_RXNE) {
				char junk = USART1->DATAR & 0xFF; 
			}
			buf_ptr = 0;
			Serial.println("I'm alive");
			uart_sent_this_cycle = true;
		}

		// BYPASS ARDUINO CORE: Read hardware directly
		if (USART1->STATR & USART_STATR_RXNE) {
			char c = USART1->DATAR & 0xFF; 
			
			if (c == '\n' || c == '\r') {
				if (buf_ptr > 0) {
					cmd_buffer[buf_ptr] = '\0';
					processUartCommand(cmd_buffer);
					buf_ptr = 0;
				}
			} else if (buf_ptr < 31) {
				cmd_buffer[buf_ptr++] = c;
			}
		}

		if (now - phase_start_ms >= UART_DURATION) {
			current_state = BITBUS_PHASE;
			phase_start_ms = now;
			blink_sent_this_cycle = false;
		}
	}
}