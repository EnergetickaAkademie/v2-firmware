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

#define RX_BUF_SIZE 256
volatile char rx_buffer[RX_BUF_SIZE];
volatile uint16_t rx_head = 0;
volatile uint16_t rx_tail = 0;

bool parseUint8List(char* input, uint8_t* output, uint8_t count, uint8_t max_value) {
	char* p = input;
	for (uint8_t i = 0; i < count; i++) {
		while (*p == ' ') p++;
		if (*p < '0' || *p > '9') return false;

		uint16_t value = 0;
		while (*p >= '0' && *p <= '9') {
			const uint8_t digit = static_cast<uint8_t>(*p - '0');
			if (value > (max_value - digit) / 10) return false;
			value = value * 10 + digit;
			p++;
		}
		if (*p != '\0' && *p != ' ') return false;
		output[i] = value;
	}

	while (*p == ' ') p++;
	return *p == '\0';
}

void printCounts() {
	uint8_t counts[8];
	bus.getActiveCountsByType(counts, 8);
	Serial.print("COUNTS");
	for (uint8_t i = 0; i < 8; i++) {
		Serial.print(" ");
		Serial.print(counts[i]);
	}
	Serial.println();
}

extern "C" void USART1_IRQHandler(void) __attribute__((interrupt));
extern "C" void USART1_IRQHandler(void) {
	if (USART1->STATR & USART_STATR_RXNE) {
		char c = USART1->DATAR & 0xFF;
		uint16_t next_head = (rx_head + 1) % RX_BUF_SIZE;
		if (next_head != rx_tail) {
			rx_buffer[rx_head] = c;
			rx_head = next_head;
		}
	}
}

void processUartCommand(char* cmd) {
	comm_led_state = !comm_led_state;
	digitalWrite(LED_PIN, comm_led_state ? LOW : HIGH);
	comm_led_timer = millis();

	if (strcmp(cmd, "PING?") == 0) {
		Serial.println("PONG");
	}
	else if (strcmp(cmd, "COUNTS?") == 0) {
		printCounts();
	}
	else if (strncmp(cmd, "BLINK TYPE ", 11) == 0) {
		int t = atoi(cmd + 11);
		if (t > 0) {
			bus.sendCommandToType(t, CMD_LED_BLINK, nullptr, 0);
		}
	}
	else if (strncmp(cmd, "ALLRGB ", 7) == 0) {
		uint8_t values[24];
		if (parseUint8List(cmd + 7, values, 24, 255)) {
			for (uint8_t type = 1; type <= 8; type++) {
				memcpy(rgb_payloads[type], &values[(type - 1) * 3], 3);
			}
			Serial.println("ACK_RGB");
		} else {
			Serial.println("ERR_RGB");
		}
	}
	else if (strncmp(cmd, "ALLMOT ", 7) == 0) {
		uint8_t values[8];
		if (parseUint8List(cmd + 7, values, 8, 100)) {
			for (uint8_t type = 1; type <= 8; type++) {
				motor_payloads[type] = values[type - 1];
			}
			Serial.println("ACK_MOT");
		} else {
			Serial.println("ERR_MOT");
		}
	}
}

void setup() {
	Serial.begin(115200);
	pinMode(LED_PIN, OUTPUT);
	digitalWrite(LED_PIN, LOW);

	USART1->CTLR1 |= USART_CTLR1_RXNEIE; //enable RX Not Empty Interrupt on peripheral

	NVIC_EnableIRQ(USART1_IRQn); //enable interrupt in NVIC

	bus.begin();
	phase_start_ms = millis();
}

void loop() {
	uint32_t now = millis();

	//drain interrupt buffer
	while (rx_tail != rx_head) {
		char c = rx_buffer[rx_tail];
		rx_tail = (rx_tail + 1) % RX_BUF_SIZE;

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
					bus.sendCommandToType(bitbus_scan_type, CMD_MOTOR_ON,
						&motor_payloads[bitbus_scan_type], 1);
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
			printCounts();
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
