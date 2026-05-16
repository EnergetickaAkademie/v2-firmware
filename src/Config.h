#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

#ifndef WIFI_SSID
	#define WIFI_SSID "YOUR_SSID"
#endif

#ifndef WIFI_PASS
	#define WIFI_PASS "YOUR_PASSWORD"
#endif

#ifndef API_BASE_URL
	#define API_BASE_URL "http://192.168.1.100/coreapi"
#endif

#ifndef BOARD_USERNAME
	#define BOARD_USERNAME "board1"
#endif

#ifndef BOARD_PASSWORD
	#define BOARD_PASSWORD "board123"
#endif

#define OUT_LATCH_PIN 10
#define OUT_DATA_PIN  11
#define SHARED_CLOCK_PIN 12
#define IN_DATA_PIN 13
#define IN_LOAD_PIN 14

#define STATUS_LED_PIN 38

#define SUB1_RX_PIN 43
#define SUB1_TX_PIN 44
#define SUB2_RX_PIN 17
#define SUB2_TX_PIN 18
#define SUB3_RX_PIN 9
#define SUB3_TX_PIN 8

#define SUBSTATION_UART_BAUD 9600
#define BARGRAPH_LED_COUNT 10
#define DISPLAY_DIGIT_COUNT 8
#define DEVICE_COUNT 6
#define INPUT_REGISTER_COUNT 3

#define SDA_PIN 1 
#define SCL_PIN 2
#define BUZZER_PIN 6

#endif