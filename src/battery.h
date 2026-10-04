#pragma once

#include <Arduino.h>

uint32_t batteryMillivolts();

// Rough state of charge from a resting LiPo voltage curve. Reads high while charging.
uint8_t batteryPercent(uint32_t millivolts);
