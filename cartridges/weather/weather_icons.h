#pragma once

#include <Adafruit_GFX.h>

// Weather icons drawn from primitives for a WMO weather code (Open-Meteo's weather_code),
// centred on (cx, cy) in a size x size box. At 32 px and up clouds are outlined; smaller
// ones are solid so they still read on the e-paper.
namespace icons {

void drawWeather(Adafruit_GFX &gfx, int code, bool isDay, int16_t cx, int16_t cy, int16_t size);

}  // namespace icons
