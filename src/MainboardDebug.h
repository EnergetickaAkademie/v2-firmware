#ifndef MAINBOARD_DEBUG_H
#define MAINBOARD_DEBUG_H

#include <Arduino.h>

bool setDebugEncoderValue(uint8_t index, int32_t value);
bool getDebugEncoderRange(uint8_t index, int32_t& minimum, int32_t& maximum);

#endif
