#include "weather_data.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD_MMC.h>

#include "log.h"
#include "net.h"
#include "storage.h"

namespace weather {
namespace {

constexpr uint32_t kRefreshMs = 60 * 60 * 1000;  // hourly
constexpr uint32_t kRetryMs = 10 * 60 * 1000;

Forecast data;
Settings prefs;

// Shared with the fetch task.
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
volatile bool running = false;
volatile bool fetched = false;
volatile time_t pendingClock = 0;
char lastError[64] = "";
uint32_t lastGood = 0, lastTry = 0;  // millis
bool everTried = false;

String path(const char *name) {
  return storage::myDataDir() + "/" + name;
}

void setError(const char *message) {
  portENTER_CRITICAL(&lock);
  strlcpy(lastError, message, sizeof(lastError));
  portEXIT_CRITICAL(&lock);
}

float num(JsonVariantConst v) {
  return v.isNull() ? NAN : v.as<float>();
}

// The API's answer (saved on the card as is) → `data`.
bool parse(const String &body, const String &place, uint32_t fetchedAt, bool imperial) {
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok || !doc["current"].is<JsonObjectConst>()) return false;
  Forecast f;
  JsonObjectConst c = doc["current"];
  f.now.temp = num(c["temperature_2m"]);
  f.now.feels = num(c["apparent_temperature"]);
  f.now.humidity = c["relative_humidity_2m"] | -1;
  f.now.code = c["weather_code"] | -1;
  f.now.wind = num(c["wind_speed_10m"]);
  f.now.windDir = c["wind_direction_10m"] | 0;
  f.now.uv = num(c["uv_index"]);
  f.now.isDay = (c["is_day"] | 1) == 1;
  JsonObjectConst d = doc["daily"];
  for (int i = 0; i < kDays; i++) {
    Day &day = f.days[i];
    strlcpy(day.date, d["time"][i] | "", sizeof(day.date));
    day.code = d["weather_code"][i] | -1;
    day.tmin = num(d["temperature_2m_min"][i]);
    day.tmax = num(d["temperature_2m_max"][i]);
    day.rainChance = d["precipitation_probability_max"][i] | -1;
    day.rain = num(d["precipitation_sum"][i]);
    day.uvMax = num(d["uv_index_max"][i]);
    day.windMax = num(d["wind_speed_10m_max"][i]);
    day.windDir = d["wind_direction_10m_dominant"][i] | 0;
    const char *rise = d["sunrise"][i] | "", *set = d["sunset"][i] | "";
    if (strlen(rise) >= 16) strlcpy(day.sunrise, rise + 11, sizeof(day.sunrise));  // "2026-10-05T07:42"
    if (strlen(set) >= 16) strlcpy(day.sunset, set + 11, sizeof(day.sunset));
  }
  f.valid = true;
  f.place = place;
  f.fetchedAt = fetchedAt;
  f.imperial = imperial;
  data = f;
  return true;
}

bool loadCache() {
  File meta = SD_MMC.open(path("forecast.meta"));
  File body = SD_MMC.open(path("forecast.json"));
  if (!meta || !body) return false;
  JsonDocument m;
  if (deserializeJson(m, meta) != DeserializationError::Ok) return false;
  return parse(body.readString(), m["place"] | "", m["fetchedAt"] | 0u, m["imperial"] | false);
}

String forecastUrl(float lat, float lon, bool imperial) {
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) + "&longitude=" + String(lon, 4) +
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,"
               "wind_direction_10m,uv_index,is_day"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
               "precipitation_sum,sunrise,sunset,uv_index_max,wind_speed_10m_max,wind_direction_10m_dominant"
               "&forecast_days=" + String(kDays) + "&timezone=auto";
  if (imperial) url += "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch";
  return url;
}

// Location → forecast → internet time → card. Runs as its own task (core 0).
void fetchTask(void *) {
  const Settings s = prefs;
  String error, place = s.name;
  float lat = s.lat, lon = s.lon;
  bool ok = net::connect(error);
  if (ok && (s.automatic || isnan(lat))) {
    String body;
    JsonDocument where;
    ok = net::getString("https://ipwho.is/?fields=success,city,latitude,longitude", body, error) &&
         deserializeJson(where, body) == DeserializationError::Ok && (where["success"] | false);
    if (ok) {
      lat = where["latitude"] | 0.0f;
      lon = where["longitude"] | 0.0f;
      place = where["city"] | "Here";
    } else if (error.isEmpty()) {
      error = "couldn't find where Dotty is";
    }
  }
  String body;
  if (ok) ok = net::getString(forecastUrl(lat, lon, s.imperial), body, error);
  JsonDocument head;
  if (ok) {
    // Only the offset is needed here; parse() does the rest on the main loop.
    JsonDocument filter;
    filter["utc_offset_seconds"] = true;
    filter["current"] = true;
    ok = deserializeJson(head, body, DeserializationOption::Filter(filter)) == DeserializationError::Ok &&
         head["current"].is<JsonObjectConst>();
    if (!ok) error = "the forecast came back empty";
  }
  if (ok) {
    time_t utc;
    String timeError;
    uint32_t local = time(nullptr);
    if (net::internetTime(utc, timeError)) {
      local = utc + (head["utc_offset_seconds"] | 0);
      pendingClock = local;
    } else {
      LOGW("weather", "clock not set: %s", timeError.c_str());
    }
    File out = SD_MMC.open(path("forecast.json"), FILE_WRITE);
    if (out) out.print(body);
    out.close();
    JsonDocument m;
    m["place"] = place;
    m["fetchedAt"] = local;
    m["imperial"] = s.imperial;
    File meta = SD_MMC.open(path("forecast.meta"), FILE_WRITE);
    if (meta) serializeJson(m, meta);
    LOGI("weather", "%s: %d bytes", place.c_str(), body.length());
  }
  net::disconnect();
  setError(ok ? "" : error.c_str());
  if (!ok) LOGW("weather", "fetch failed: %s", error.c_str());
  fetched = ok;
  running = false;
  vTaskDelete(nullptr);
}

}  // namespace

void begin() {
  Preferences p;
  p.begin("weather", true);
  prefs.automatic = p.getBool("auto", true);
  prefs.lat = p.getFloat("lat", NAN);
  prefs.lon = p.getFloat("lon", NAN);
  prefs.name = p.getString("name", "");
  prefs.source = p.getString("source", "");
  prefs.imperial = p.getBool("imperial", false);
  p.end();
  storage::makeDirs(storage::myDataDir());
  if (loadCache()) LOGI("weather", "cached forecast for %s", data.place.c_str());
}

const Forecast &forecast() {
  return data;
}

Settings settings() {
  return prefs;
}

void setLocation(bool automatic, float lat, float lon, const String &name, const String &source) {
  prefs.automatic = automatic;
  prefs.lat = lat;
  prefs.lon = lon;
  prefs.name = name;
  prefs.source = automatic ? String() : source;
  Preferences p;
  p.begin("weather", false);
  p.putBool("auto", automatic);
  p.putFloat("lat", lat);
  p.putFloat("lon", lon);
  p.putString("name", name);
  p.putString("source", prefs.source);
  p.end();
  everTried = false;  // fetch for the new place right away
}

void setImperial(bool imperial) {
  prefs.imperial = imperial;
  Preferences p;
  p.begin("weather", false);
  p.putBool("imperial", imperial);
  p.end();
  everTried = false;
}

FetchState fetchState() {
  FetchState s;
  s.running = running;
  char error[sizeof(lastError)];
  portENTER_CRITICAL(&lock);
  memcpy(error, lastError, sizeof(error));
  portEXIT_CRITICAL(&lock);
  s.error = error;
  return s;
}

bool fetchDue() {
  if (running || net::saved().empty()) return false;
  if (!everTried) return true;
  const uint32_t now = millis();
  if (lastGood && now - lastGood < kRefreshMs) return false;
  return now - lastTry >= (lastError[0] ? kRetryMs : kRefreshMs);
}

void startFetch() {
  if (running) return;
  everTried = true;
  lastTry = millis();
  running = true;
  fetched = false;
  if (xTaskCreatePinnedToCore(fetchTask, "weather", 12288, nullptr, 3, nullptr, 0) != pdPASS) {
    running = false;
    setError("not enough memory");
  }
}

bool takeFetched() {
  if (!fetched) return false;
  fetched = false;
  lastGood = millis();
  return loadCache();
}

time_t takeClock() {
  const time_t t = pendingClock;
  pendingClock = 0;
  return t;
}

const char *describe(int code) {
  switch (code) {
    case 0: return "Clear";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: case 73: case 75: case 77: return "Snow";
    case 80: case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunder, hail";
    default: return "";
  }
}

}  // namespace weather
