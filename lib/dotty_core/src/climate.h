#pragma once

#include <Arduino.h>

// The SHTC3 room temperature and humidity sensor (I2C 0x70, always powered; the audio
// rail must be on for the shared bus, which the shell takes care of while awake).
// The board warms itself a little while busy, so readings taken right after waking from
// sleep are the most honest.
namespace climate {

struct Reading {
  float celsius = 0;
  float humidity = 0;  // % relative
};

// One measurement (~15 ms, then the sensor sleeps again). False if it didn't answer.
bool read(Reading &out);

}  // namespace climate
