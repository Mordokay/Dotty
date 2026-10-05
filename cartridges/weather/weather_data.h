#pragma once

#include <Arduino.h>
#include <time.h>

// The forecast from Open-Meteo (https://open-meteo.com: free, no key) for a place that is
// either found automatically from Dotty's internet connection (ipwho.is) or set from the
// app (the iPhone's location or a searched city). Downloaded hourly in a background task,
// kept on the SD card (data/forecast.json) so it shows straight away after a restart.
// Each download also brings internet time, so Dotty's clock stays exact.
namespace weather {

constexpr int kDays = 7;

struct Now {
  float temp = NAN, feels = NAN;  // in the chosen units
  int humidity = -1;              // %
  int code = -1;                  // WMO weather code
  float wind = NAN;               // km/h or mph
  int windDir = 0;                // degrees the wind comes from
  float uv = NAN;
  bool isDay = true;
};

struct Day {
  char date[11] = "";  // YYYY-MM-DD
  int code = -1;
  float tmin = NAN, tmax = NAN;
  int rainChance = -1;  // %
  float rain = NAN;     // mm or inch
  float uvMax = NAN;
  char sunrise[6] = "", sunset[6] = "";  // HH:MM local
};

struct Forecast {
  bool valid = false;
  uint32_t fetchedAt = 0;  // local time
  String place;
  bool imperial = false;
  Now now;
  Day days[kDays];
};

// Settings (NVS "weather"). Location: automatic, or fixed at lat/lon with a name.
struct Settings {
  bool automatic = true;
  float lat = NAN, lon = NAN;
  String name;
  bool imperial = false;
  float insideOffset = 0;  // °C added to the room sensor's reading
};

void begin();  // settings + the cached forecast
const Forecast &forecast();
Settings settings();
void setLocation(bool automatic, float lat, float lon, const String &name);
void setImperial(bool imperial);
void setInsideOffset(float celsius);

struct FetchState {
  bool running = false;
  String error;  // last failure, "" if the last fetch worked
};
FetchState fetchState();
bool fetchDue();  // an hour since the last good fetch (10 min after a failure)
void startFetch();

// Main loop, after a fetch: true once when the forecast changed (it's been reloaded).
bool takeFetched();
// Main loop: internet time to apply (local epoch), once; 0 if none.
time_t takeClock();

// "Showers", "Partly cloudy"… for a WMO code.
const char *describe(int code);

}  // namespace weather
