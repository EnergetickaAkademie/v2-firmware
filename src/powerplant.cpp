#include <Arduino.h>
#include "slave.h"
#include "PeripheralFactory.h"

// Hardware Pin Definitions
#define LED_PIN PA2          // Status LED
#define RGB_PIN PC0          // NeoPixel Data Pin
#define MOTOR_PIN_A PC4      // Motor Driver Input 1
#define MOTOR_PIN_B PC3      // Motor Driver Input 2

#ifndef DEVICE_TYPE
#define DEVICE_TYPE TYPE_UNKNOWN
#endif

#ifndef DEVICE_UID
#define DEVICE_UID 0x00000000
#endif

Motor* motor = nullptr;

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
			if(DEVICE_TYPE == TYPE_WIND or DEVICE_TYPE == TYPE_HYDRO){
				//myRGB->setColor(100, 100, 100);
				//motor->forward(700);
				// For MOTOR_PIN_A (PC4 / TIM1_CH1)
				TIM_SetCompare1(TIM1, 100); 

				// For MOTOR_PIN_B (PC5 / TIM1_CH3)
				TIM_SetCompare3(TIM1, 0);
			}
			
			else{
				digitalWrite(MOTOR_PIN_A, HIGH);
				digitalWrite(MOTOR_PIN_B, LOW);
			}

			break;

		case CMD_MOTOR_OFF:
			if(DEVICE_TYPE == TYPE_WIND or DEVICE_TYPE == TYPE_HYDRO){
				//motor->stop();
				// For MOTOR_PIN_A (PC4 / TIM1_CH1)
				TIM_SetCompare1(TIM1, 0); 

				// For MOTOR_PIN_B (PC5 / TIM1_CH3)
				TIM_SetCompare3(TIM1, 0);
			}
		
			else{
				digitalWrite(MOTOR_PIN_A, LOW);
				digitalWrite(MOTOR_PIN_B, LOW);
			}
			
			break;
	}
}

void setupMotorPWM() {
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1 | RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO, ENABLE);

	GPIO_PinRemapConfig(GPIO_FullRemap_TIM1, ENABLE);

	GPIO_InitTypeDef GPIO_InitStructure = {0};
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4 | GPIO_Pin_5;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOC, &GPIO_InitStructure);

	TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure = {0};
	TIM_TimeBaseStructure.TIM_Period = 255;
	TIM_TimeBaseStructure.TIM_Prescaler = (48000000 / 1000 / 256) - 1; 
	TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
	TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
	TIM_TimeBaseInit(TIM1, &TIM_TimeBaseStructure);

	TIM_OCInitTypeDef TIM_OCInitStructure = {0};
	TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
	TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
	TIM_OCInitStructure.TIM_Pulse = 0;
	TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_High;

	TIM_OC1Init(TIM1, &TIM_OCInitStructure);
	TIM_OC1PreloadConfig(TIM1, TIM_OCPreload_Enable);

	TIM_OC3Init(TIM1, &TIM_OCInitStructure);
	TIM_OC3PreloadConfig(TIM1, TIM_OCPreload_Enable);

	TIM_CtrlPWMOutputs(TIM1, ENABLE);
	TIM_Cmd(TIM1, ENABLE);
}

void setup() {
	// Status LED
	pinMode(LED_PIN, OUTPUT);
	digitalWrite(LED_PIN, LOW);
	
	if(DEVICE_TYPE == TYPE_WIND or DEVICE_TYPE == TYPE_HYDRO){
		//motor = factory.createMotor(MOTOR_PIN_B, MOTOR_PIN_A, 50);
		//pinMode(MOTOR_PIN_A, OUTPUT);
		//pinMode(MOTOR_PIN_B, OUTPUT);
		//analogWrite(MOTOR_PIN_A, 0);
		//analogWrite(MOTOR_PIN_B, 0);
		setupMotorPWM();
	}


	else{
		pinMode(MOTOR_PIN_A, OUTPUT);
		pinMode(MOTOR_PIN_B, OUTPUT);
		digitalWrite(MOTOR_PIN_A, LOW);
		digitalWrite(MOTOR_PIN_B, LOW);
	}


	myRGB = factory.createSimpleRGB(RGB_PIN);
	if (myRGB != nullptr) {
		myRGB->setColor(5, 0, 5);
	}
	
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