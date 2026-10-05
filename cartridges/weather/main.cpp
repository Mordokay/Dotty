// Weather Station: outside weather from Open-Meteo (weather_data.h) next to the room's own
// temperature and humidity (SHTC3, climate.h).
//
// Screens: Today (big icon, outside temperature and condition; inside | outside; UV, wind,
// rain chance and the next sunset or sunrise) and Week (7 rows). Swipe, the nav arrows or
// BOOT switch between them. Hourly downloads also run while locked, and each one sets
// Dotty's clock from internet time. The lock screen shows a short summary.

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include "cartridge.h"
#include "climate.h"
#include "core_ble.h"
#include "images/sleep_panda.h"
#include "images/sleep_portrait.h"
#include "log.h"
#include "nav_bar.h"
#include "net.h"
#include "power.h"
#include "shell.h"
#include "storage.h"
#include "ui.h"
#include "weather_data.h"
#include "weather_icons.h"

DOTTY_CARTRIDGE("weather", "Weather Station", "0.1.1");

namespace {

using shell::epd;

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;
constexpr uint32_t kInsideEveryMs = 60 * 1000;

// Today, then the next 6 days in two pages of three.
enum class Screen { Today, Days1, Days2 };
Screen screen = Screen::Today;
bool sdReady = false;
bool redraw = true;

climate::Reading inside;
bool insideValid = false;
uint32_t insideAt = 0;

// ---------- values ----------

void readInside(bool force = false) {
  if (!force && insideValid && millis() - insideAt < kInsideEveryMs) return;
  climate::Reading r;
  if (climate::read(r)) {
    const bool changed = !insideValid || fabsf(r.celsius - inside.celsius) >= 0.1f ||
                         fabsf(r.humidity - inside.humidity) >= 1.0f;
    inside = r;
    insideValid = true;
    if (changed && screen == Screen::Today) redraw = true;
  }
  insideAt = millis();
}

bool imperial() {
  return weather::settings().imperial;
}

// The ASCII fonts have no degree sign; a small raised circle is drawn instead (see deg()).
String tempText(float v, int decimals = 0) {
  return isnan(v) ? String("--") : String(v, decimals);
}

// Draws text with a small degree circle after it; returns the x after it.
int16_t printTemp(Adafruit_GFX &gfx, int16_t x, int16_t y, float v, int decimals, bool big = false) {
  const String text = tempText(v, decimals);
  gfx.setCursor(x, y);
  gfx.print(text);
  int16_t x1, y1;
  uint16_t w, h;
  gfx.getTextBounds(text.c_str(), x, y, &x1, &y1, &w, &h);
  const int16_t r = big ? 4 : 2;
  const int16_t cx = x1 + w + r + (big ? 3 : 2), cy = y1 + r;
  if (!isnan(v)) {
    gfx.drawCircle(cx, cy, r, kBlack);
    if (big) gfx.drawCircle(cx, cy, r - 1, kBlack);
  }
  return cx + r + 2;
}

int16_t tempWidth(Adafruit_GFX &gfx, float v, int decimals, bool big = false) {
  return ui::textWidth(gfx, tempText(v, decimals)) + (big ? 14 : 8);
}

// The sensor sits on the board, which warms it a little: the app can set a correction.
float insideCelsius() {
  return inside.celsius + weather::settings().insideOffset;
}

float insideTemp() {
  return imperial() ? insideCelsius() * 9 / 5 + 32 : insideCelsius();
}

const char *compass(int deg) {
  static const char *kPoints[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return kPoints[((deg + 22) % 360) / 45];
}

const char *uvLabel(float uv) {
  if (uv < 3) return "low";
  if (uv < 6) return "mod";
  if (uv < 8) return "high";
  if (uv < 11) return "v.high";
  return "extr";
}

// "Sunset 19:13" while it's day, "Sunrise 07:42" after dark (today's or tomorrow's).
String sunLine(const weather::Forecast &f) {
  tm now;
  if (!shell::rtc.read(now)) return "";
  char hhmm[6];
  snprintf(hhmm, sizeof(hhmm), "%02d:%02d", now.tm_hour, now.tm_min);
  const weather::Day &today = f.days[0];
  if (strcmp(hhmm, today.sunrise) < 0) return String("Sunrise ") + today.sunrise;
  if (strcmp(hhmm, today.sunset) < 0) return String("Sunset ") + today.sunset;
  return String("Sunrise ") + f.days[1].sunrise;
}

// "Mon".. from YYYY-MM-DD; "Today" for the first row.
String dayName(const char *date, int index) {
  if (index == 0) return "Today";
  tm t = {};
  if (sscanf(date, "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) != 3) return "";
  t.tm_year -= 1900;
  t.tm_mon -= 1;
  t.tm_hour = 12;
  mktime(&t);
  static const char *kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  return kDays[t.tm_wday % 7];
}

// "3h ago" when the forecast is old (no internet for a while), else "".
String age(const weather::Forecast &f) {
  const uint32_t now = time(nullptr);
  if (!f.fetchedAt || now < f.fetchedAt) return "";
  const uint32_t hours = (now - f.fetchedAt) / 3600;
  return hours >= 2 ? String(hours) + "h ago" : String();
}

// ---------- screens ----------

void drawToday() {
  const weather::Forecast &f = weather::forecast();
  epd.fillScreen(kWhite);
  const String stale = age(f);
  const String place = f.place.length() ? f.place : String("Weather");
  nav::draw(epd, stale.length() ? place + " - " + stale : place, nav::Icon::None, nav::Icon::Forward);
  epd.setTextColor(kBlack);

  // Top: the weather now.
  if (f.valid) {
    icons::drawWeather(epd, f.now.code, f.now.isDay, 38, 64, 56);
    epd.setFont(&FreeSansBold24pt7b);
    printTemp(epd, 80, 72, f.now.temp, 0, true);
    epd.setFont(&FreeSans9pt7b);
    epd.setCursor(80, 92);
    epd.print(ui::fitText(epd, weather::describe(f.now.code), kW - 84));
  } else {
    epd.setFont(&FreeSansBold9pt7b);
    const weather::FetchState s = weather::fetchState();
    ui::drawCentered(epd, s.running ? "Getting the weather..." : "No forecast yet", 62);
    epd.setFont(&FreeSans9pt7b);
    if (!s.running) {
      ui::drawCentered(epd, net::saved().empty() ? "Add Wi-Fi in the app" : ui::fitText(epd, s.error, kW - 16), 84);
    }
  }

  // Inside | outside.
  epd.drawFastHLine(6, 98, kW - 12, kBlack);
  epd.drawFastVLine(kW / 2, 102, 38, kBlack);
  epd.setFont(&FreeSans9pt7b);
  const char *labels[2] = {"INSIDE", "OUTSIDE"};
  for (int col = 0; col < 2; col++) {
    const int16_t left = col * kW / 2, mid = left + kW / 4;
    epd.setCursor(mid - ui::textWidth(epd, labels[col]) / 2, 116);
    epd.print(labels[col]);
    const bool have = col == 0 ? insideValid : f.valid;
    const float t = col == 0 ? insideTemp() : f.now.temp;
    const int hum = col == 0 ? lroundf(inside.humidity) : f.now.humidity;
    epd.setFont(&FreeSansBold9pt7b);
    const String humText = have && hum >= 0 ? "  " + String(hum) + "%" : String();
    const int16_t width = tempWidth(epd, have ? t : NAN, 1) + ui::textWidth(epd, humText);
    const int16_t x = printTemp(epd, mid - width / 2, 136, have ? t : NAN, 1);
    epd.setCursor(x, 136);
    epd.print(humText);
    epd.setFont(&FreeSans9pt7b);
  }

  // UV, wind, rain, sun.
  epd.drawFastHLine(6, 144, kW - 12, kBlack);
  // Left column narrower: "UV 1 low" / "Rain 8%" are short, "SW 14 km/h" / "Sunset 19:13" aren't.
  constexpr int16_t kSplit = 84;
  epd.drawFastVLine(kSplit, 148, 46, kBlack);
  if (f.valid) {
    const weather::Day &today = f.days[0];
    const String cells[4] = {
        isnan(f.now.uv) ? String("UV --") : "UV " + String(lroundf(f.now.uv)) + " " + uvLabel(f.now.uv),
        isnan(f.now.wind) ? String("Wind --")
                          : String(compass(f.now.windDir)) + " " + String(lroundf(f.now.wind)) +
                                (f.imperial ? " mph" : " km/h"),
        today.rainChance < 0 ? String("Rain --") : "Rain " + String(today.rainChance) + "%",
        sunLine(f),
    };
    for (int i = 0; i < 4; i++) {
      const int16_t left = i % 2 ? kSplit : 0, width = i % 2 ? kW - kSplit : kSplit;
      const int16_t mid = left + width / 2;
      const String text = ui::fitText(epd, cells[i], width - 6);
      epd.setCursor(mid - ui::textWidth(epd, text) / 2, i < 2 ? 165 : 189);
      epd.print(text);
    }
  }
}

// Width of text as drawn (ui::textWidth would also squash double spaces).
int16_t rawWidth(Adafruit_GFX &gfx, const String &text) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx.getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);
  return w;
}

// A small raindrop, about 6 x 9 px, its tip at (x, top).
void drop(Adafruit_GFX &gfx, int16_t x, int16_t top) {
  gfx.fillTriangle(x, top, x - 3, top + 5, x + 3, top + 5, kBlack);
  gfx.fillCircle(x, top + 6, 3, kBlack);
}

// A day's total rain in words: people don't know what 1.6 mm looks like. Daily totals, so
// the bands are wider than the usual hourly ones (light < 2.5 mm/h).
const char *rainAmount(float amount, bool imperial) {
  if (isnan(amount)) return "";
  const float mm = imperial ? amount * 25.4f : amount;
  if (mm < 0.2f) return "Dry";
  if (mm < 4) return "Light";
  if (mm < 15) return "Moderate";
  return "Heavy";
}

// Three days per page, each a card: a big icon on the left, two lines on the right.
//   Tue 6           19/23°
//   (drop) 28% Light
void drawDays(int page) {
  const weather::Forecast &f = weather::forecast();
  epd.fillScreen(kWhite);
  nav::draw(epd, "Next days " + String(page + 1) + "/2", nav::Icon::Back,
            page == 0 ? nav::Icon::Forward : nav::Icon::None);
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSans9pt7b);
  if (!f.valid) {
    ui::drawCentered(epd, "No forecast yet", 110);
    return;
  }
  constexpr int16_t kRowH = 56, kText = 62;
  for (int row = 0; row < 3; row++) {
    const int index = 1 + page * 3 + row;  // tomorrow onwards
    if (index >= weather::kDays) break;
    const weather::Day &d = f.days[index];
    const int16_t top = nav::kHeight + 2 + row * kRowH;
    icons::drawWeather(epd, d.code, true, 30, top + kRowH / 2, 44);

    // Line 1: day and date, low/high on the right.
    epd.setFont(&FreeSansBold9pt7b);
    const char *dayOfMonth = strlen(d.date) == 10 ? d.date + 8 : "";
    epd.setCursor(kText, top + 22);
    epd.print(dayName(d.date, index) + " " + String(atoi(dayOfMonth)));
    const String low = isnan(d.tmin) ? String("--") : String(lroundf(d.tmin));
    const String high = isnan(d.tmax) ? String("--") : String(lroundf(d.tmax));
    const String temps = low + "/" + high;
    epd.setCursor(kW - 6 - 7 - ui::textWidth(epd, temps), top + 22);
    epd.print(temps);
    if (!isnan(d.tmax)) epd.drawCircle(epd.getCursorX() + 3, top + 11, 2, kBlack);

    // Line 2: chance of rain and how much, in words.
    epd.setFont(&FreeSans9pt7b);
    drop(epd, kText + 3, top + 31);
    epd.setCursor(kText + 10, top + 44);
    epd.print((d.rainChance < 0 ? String("--") : String(d.rainChance) + "%") + " " +
              rainAmount(d.rain, f.imperial));

    if (row < 2) {
      for (int16_t dx = 6; dx < kW - 6; dx += 4) epd.drawPixel(dx, top + kRowH - 1, kBlack);  // dotted line
    }
  }
}

void drawApp() {
  readInside();
  if (screen == Screen::Today) drawToday();
  else drawDays(screen == Screen::Days1 ? 0 : 1);
}

// ---------- lock screen ----------


// A card like the forecast rows: the weather icon on the left, up to three lines beside it.
//   23° out  29° in
//   Rain 8%
//   Sunset 19:13
int16_t drawLockSummary(Adafruit_GFX &gfx, const tm &, int16_t maxHeight) {
  const weather::Forecast &f = weather::forecast();
  readInside();
  if (!f.valid && !insideValid) return 0;
  constexpr int16_t kIcon = 48, kGap = 10, kDeg = 7, kSpace = 12, kLine = 19;
  gfx.setFont(&FreeSansBold9pt7b);
  const String out = f.valid ? tempText(f.now.temp, 0) : String("--");
  const String in = insideValid ? tempText(insideTemp(), 0) : String("--");
  const int16_t line1 = rawWidth(gfx, out) + kDeg + rawWidth(gfx, " out") + kSpace + rawWidth(gfx, in) + kDeg +
                        rawWidth(gfx, " in");
  gfx.setFont(&FreeSans9pt7b);
  const String rain = f.valid && f.days[0].rainChance >= 0 ? "Rain " + String(f.days[0].rainChance) + "%" : String();
  const String sun = f.valid ? sunLine(f) : String();
  const int16_t textW = max(line1, max(rawWidth(gfx, rain), rawWidth(gfx, sun)));
  const int16_t lines = 1 + (rain.length() ? 1 : 0) + (sun.length() ? 1 : 0);
  const int16_t height = max<int16_t>(kIcon, lines * kLine);
  // Icon and text centred together, as one card.
  int16_t x = (kW - (f.valid ? kIcon + kGap : 0) - textW) / 2;
  if (f.valid) {
    icons::drawWeather(gfx, f.now.code, f.now.isDay, x + kIcon / 2, height / 2, kIcon);
    x += kIcon + kGap;
  }
  int16_t y = (height - lines * kLine) / 2 + 14;
  gfx.setFont(&FreeSansBold9pt7b);
  int16_t cx = x;
  auto part = [&](const String &value, const char *label) {
    gfx.setCursor(cx, y);
    gfx.print(value);
    cx += rawWidth(gfx, value);
    gfx.drawCircle(cx + 3, y - 11, 2, kBlack);  // degree sign
    cx += kDeg;
    gfx.setCursor(cx, y);
    gfx.print(label);
    cx += rawWidth(gfx, label);
  };
  part(out, " out");
  cx += kSpace;
  part(in, " in");
  gfx.setFont(&FreeSans9pt7b);
  for (const String *text : {&rain, &sun}) {
    if (text->isEmpty()) continue;
    y += kLine;
    gfx.setCursor(x, y);
    gfx.print(*text);
  }
  return min<int16_t>(maxHeight, height);
}

// ---------- touch ----------

void onGesture(Touch::Gesture g, uint16_t x, uint16_t y) {
  using G = Touch::Gesture;
  const int hit = g == G::Tap ? nav::hit(x, y) : 0;
  const bool next = g == G::SwipeLeft || hit == 1, back = g == G::SwipeRight || hit == -1;
  if (next && screen == Screen::Today) screen = Screen::Days1;
  else if (next && screen == Screen::Days1) screen = Screen::Days2;
  else if (back && screen == Screen::Days2) screen = Screen::Days1;
  else if (back && screen == Screen::Days1) screen = Screen::Today;
  else return;
  redraw = true;
}

// ---------- app (BLE) ----------

void notifyChanged() {
  JsonDocument event;
  event["event"] = "weather.changed";
  ble::notify(event);
}

void registerCommands() {
  ble::on("weather.status", [](JsonObjectConst, JsonObject reply) {
    const weather::Settings s = weather::settings();
    JsonObject loc = reply["location"].to<JsonObject>();
    loc["automatic"] = s.automatic;
    if (!s.automatic) {
      loc["name"] = s.name;
      loc["lat"] = s.lat;
      loc["lon"] = s.lon;
    }
    reply["units"] = s.imperial ? "imperial" : "metric";
    reply["insideOffset"] = s.insideOffset;
    readInside(true);
    if (insideValid) {
      reply["inside"]["temp"] = roundf(insideTemp() * 10) / 10;
      reply["inside"]["humidity"] = lroundf(inside.humidity);
    }
    const weather::Forecast &f = weather::forecast();
    if (f.valid) {
      JsonObject now = reply["now"].to<JsonObject>();
      now["place"] = f.place;
      now["temp"] = f.now.temp;
      now["feels"] = f.now.feels;
      now["humidity"] = f.now.humidity;
      now["code"] = f.now.code;
      now["description"] = weather::describe(f.now.code);
      now["wind"] = f.now.wind;
      now["uv"] = f.now.uv;
      now["fetchedAt"] = f.fetchedAt;
    }
    const weather::FetchState st = weather::fetchState();
    reply["fetching"] = st.running;
    if (st.error.length()) reply["error"] = st.error;
  });
  // {automatic: true} or {automatic: false, lat, lon, name}
  ble::on("weather.location", [](JsonObjectConst args, JsonObject reply) {
    const bool automatic = args["automatic"] | true;
    const float lat = args["lat"] | NAN, lon = args["lon"] | NAN;
    if (!automatic && (isnan(lat) || isnan(lon))) {
      reply["ok"] = false;
      reply["error"] = "lat and lon are needed";
      return;
    }
    weather::setLocation(automatic, lat, lon, args["name"] | "");
  });
  // {offset}: °C added to the room sensor's reading (the board warms it a little).
  ble::on("weather.inside.offset", [](JsonObjectConst args, JsonObject) {
    weather::setInsideOffset(args["offset"] | 0.0f);
    redraw = true;
  });
  ble::on("weather.units", [](JsonObjectConst args, JsonObject) {
    weather::setImperial(strcmp(args["units"] | "metric", "imperial") == 0);
    redraw = true;
  });
  ble::on("weather.refresh", [](JsonObjectConst, JsonObject reply) {
    if (net::saved().empty()) {
      reply["ok"] = false;
      reply["error"] = "Dotty has no Wi-Fi yet";
      return;
    }
    weather::startFetch();
  });
}

const shell::Picture kOffPictures[] = {
    {kSleepPortrait, kSleepPortraitWidth, kSleepPortraitHeight},
    {kSleepPanda, kSleepPandaWidth, kSleepPandaHeight},
};

}  // namespace

void setup() {
  shell::Config config;
  config.drawApp = drawApp;
  config.drawLockWidget = drawLockSummary;
  config.offPictures = kOffPictures;
  config.offPictureCount = sizeof(kOffPictures) / sizeof(kOffPictures[0]);
  shell::begin(config);

  sdReady = storage::begin();
  if (sdReady) weather::begin();
  readInside(true);
  registerCommands();
  shell::showApp();
}

void loop() {
  // Downloads (hourly, also while locked: the wake lock keeps Dotty up until it's done).
  power::setWakeLock(power::kWakeLockNetwork, weather::fetchState().running);

  shell::Input input;
  const bool unlocked = shell::update(input);

  if (sdReady && weather::fetchDue()) {
    weather::startFetch();
    redraw = true;
  }
  if (weather::takeFetched()) {
    notifyChanged();
    redraw = true;
  }
  if (const time_t local = weather::takeClock()) shell::setLocalTime(local);
  if (!unlocked) return;

  readInside();
  if (input.key == 'r') weather::startFetch();  // developer aids: fetch now…
  if (input.key == 'n') input.boot = true;       // …switch screens, like BOOT…
  if (input.key == 'l') shell::lock();           // …and lock (to screenshot the lock screen)
  if (input.gesture != Touch::Gesture::None) onGesture(input.gesture, shell::touch.x(), shell::touch.y());
  if (input.boot) {
    // Today, next days 1/2, 2/2, back to Today.
    screen = screen == Screen::Today ? Screen::Days1 : screen == Screen::Days1 ? Screen::Days2 : Screen::Today;
    redraw = true;
  }
  if (redraw && !epd.isBusy()) {
    redraw = false;
    drawApp();
    shell::refresh(false);
  }
  delay(10);
}
