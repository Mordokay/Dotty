// Weather Station: the weather from Open-Meteo (weather_data.h).
//
// Screens: Today (big icon, temperature and condition; today's low/high | feels like; UV,
// wind, rain chance and the next sunset or sunrise), then the next 6 days as cards on two
// pages. Swipe, the nav arrows or BOOT switch between them. Hourly downloads also run while
// locked, and each one sets Dotty's clock from internet time. The lock screen shows a card.
// (The board's SHTC3 read the room 5-8 °C high from Dotty's own heat, so there's no inside
// reading.)

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include "cartridge.h"
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

DOTTY_CARTRIDGE("weather", "Weather Station", "0.3.6");

namespace {

using shell::epd;

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

// Today, then the next 6 days in two pages of three.
enum class Screen { Today, Days1, Days2 };
Screen screen = Screen::Today;
bool sdReady = false;
bool redraw = true;

// ---------- values ----------

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

const char *compass(int deg) {
  static const char *kPoints[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return kPoints[((deg + 22) % 360) / 45];
}

// "SW 14 km/h"
String windText(float speed, int dir, bool imperial) {
  if (isnan(speed)) return "--";
  return String(compass(dir)) + " " + String(lroundf(speed)) + (imperial ? " mph" : " km/h");
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

// Width of text as drawn (ui::textWidth would also squash double spaces).
int16_t rawWidth(Adafruit_GFX &gfx, const String &text) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx.getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);
  return w;
}


void drawToday() {
  const weather::Forecast &f = weather::forecast();
  epd.fillScreen(kWhite);
  const String stale = age(f);
  const String place = f.place.length() ? f.place : String("Weather");
  nav::draw(epd, stale.length() ? place + " - " + stale : place, nav::Icon::None, nav::Icon::Forward);
  epd.setTextColor(kBlack);

  // Top: the weather now.
  if (f.valid) {
    icons::drawWeather(epd, f.now.code, f.now.isDay, 34, 73, 48);
    epd.setFont(&FreeSansBold24pt7b);
    printTemp(epd, 70, 83, f.now.temp, 0, true);
    epd.setFont(&FreeSans9pt7b);
    epd.setCursor(70, 101);
    epd.print(ui::fitText(epd, weather::describe(f.now.code), kW - 74));
  } else {
    epd.setFont(&FreeSansBold9pt7b);
    const weather::FetchState s = weather::fetchState();
    ui::drawCentered(epd, s.running ? "Getting the weather..." : "No forecast yet", 70);
    epd.setFont(&FreeSans9pt7b);
    if (!s.running) {
      ui::drawCentered(epd, net::saved().empty() ? "Add Wi-Fi in the app" : ui::fitText(epd, s.error, kW - 16), 92);
    }
  }

  // Today's low/high | feels like (right column wider: "FEELS LIKE" is the longest label).
  constexpr int16_t kMidSplit = 92;
  const int16_t leftMid = kMidSplit / 2, rightMid = (kMidSplit + kW) / 2;
  epd.drawFastHLine(6, 106, kW - 12, kBlack);
  epd.drawFastVLine(kMidSplit, 109, 32, kBlack);
  epd.setFont(&FreeSans9pt7b);
  epd.setCursor(leftMid - ui::textWidth(epd, "TODAY") / 2, 122);
  epd.print("TODAY");
  epd.setCursor(rightMid - ui::textWidth(epd, "FEELS LIKE") / 2, 122);
  epd.print("FEELS LIKE");
  epd.setFont(&FreeSansBold9pt7b);
  const weather::Day &today = f.days[0];
  const float low = f.valid ? today.tmin : NAN, high = f.valid ? today.tmax : NAN;
  const int16_t rangeW = tempWidth(epd, low, 0) + rawWidth(epd, " / ") + tempWidth(epd, high, 0);
  int16_t x = printTemp(epd, leftMid - rangeW / 2, 140, low, 0);
  epd.setCursor(x, 140);
  epd.print(" / ");
  printTemp(epd, epd.getCursorX(), 140, high, 0);
  const float feels = f.valid ? f.now.feels : NAN;
  printTemp(epd, rightMid - tempWidth(epd, feels, 0) / 2, 140, feels, 0);
  epd.setFont(&FreeSans9pt7b);

  // UV, wind, rain, sun.
  epd.drawFastHLine(6, 145, kW - 12, kBlack);
  // Left column narrower: "UV 1 low" / "Rain 8%" are short, "SW 14 km/h" / "Sunset 19:13" aren't.
  constexpr int16_t kSplit = 84;
  epd.drawFastVLine(kSplit, 148, 46, kBlack);
  if (f.valid) {
    const String cells[4] = {
        isnan(f.now.uv) ? String("UV --") : "UV " + String(lroundf(f.now.uv)) + " " + uvLabel(f.now.uv),
        isnan(f.now.wind) ? String("Wind --") : windText(f.now.wind, f.now.windDir, f.imperial),
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

// A small raindrop, about 6 x 9 px, its tip at (x, top).
void drop(Adafruit_GFX &gfx, int16_t x, int16_t top) {
  gfx.fillTriangle(x, top, x - 3, top + 5, x + 3, top + 5, kBlack);
  gfx.fillCircle(x, top + 6, 3, kBlack);
}

// A small wind sign, about 12 x 9 px: three streaks, the top and bottom ones curling up.
void windSign(Adafruit_GFX &gfx, int16_t x, int16_t top) {
  gfx.drawFastHLine(x, top + 1, 8, kBlack);
  gfx.drawFastHLine(x, top + 2, 8, kBlack);
  gfx.drawFastHLine(x + 2, top + 5, 10, kBlack);
  gfx.drawFastHLine(x + 2, top + 6, 10, kBlack);
  gfx.drawFastHLine(x, top + 9, 7, kBlack);
  gfx.drawFastHLine(x, top + 10, 7, kBlack);
  gfx.fillCircle(x + 9, top + 1, 1, kBlack);
  gfx.fillCircle(x + 8, top + 9, 1, kBlack);
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
//   (wind) W 22 km/h
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
  constexpr int16_t kRowH = 51, kText = 58;
  for (int row = 0; row < 3; row++) {
    const int index = 1 + page * 3 + row;  // tomorrow onwards
    if (index >= weather::kDays) break;
    const weather::Day &d = f.days[index];
    const int16_t top = nav::kHeight + 1 + row * kRowH;
    icons::drawWeather(epd, d.code, true, 27, top + kRowH / 2, 40);

    // Line 1: day and date, low/high on the right.
    epd.setFont(&FreeSansBold9pt7b);
    const char *dayOfMonth = strlen(d.date) == 10 ? d.date + 8 : "";
    epd.setCursor(kText, top + 15);
    epd.print(dayName(d.date, index) + " " + String(atoi(dayOfMonth)));
    const String low = isnan(d.tmin) ? String("--") : String(lroundf(d.tmin));
    const String high = isnan(d.tmax) ? String("--") : String(lroundf(d.tmax));
    const String temps = low + "/" + high;
    epd.setCursor(kW - 6 - 7 - ui::textWidth(epd, temps), top + 15);
    epd.print(temps);
    if (!isnan(d.tmax)) epd.drawCircle(epd.getCursorX() + 3, top + 4, 2, kBlack);

    // Line 2: chance of rain and how much, in words.
    epd.setFont(&FreeSans9pt7b);
    drop(epd, kText + 3, top + 19);
    epd.setCursor(kText + 10, top + 31);
    epd.print((d.rainChance < 0 ? String("--") : String(d.rainChance) + "%") + " " +
              rainAmount(d.rain, f.imperial));

    // Line 3: the day's strongest wind.
    windSign(epd, kText - 2, top + 36);
    epd.setCursor(kText + 14, top + 47);
    epd.print(windText(d.windMax, d.windDir, f.imperial));

    if (row < 2) {
      for (int16_t dx = 6; dx < kW - 6; dx += 4) epd.drawPixel(dx, top + kRowH - 1, kBlack);  // dotted line
    }
  }
}

void drawApp() {
  if (screen == Screen::Today) drawToday();
  else drawDays(screen == Screen::Days1 ? 0 : 1);
}

// ---------- lock screen ----------


// A card like the forecast rows: the weather icon on the left, three lines beside it.
//   23°  17/27°
//   (drop) 8% Dry
//   Sunset 19:13
int16_t drawLockSummary(Adafruit_GFX &gfx, const tm &, int16_t maxHeight) {
  const weather::Forecast &f = weather::forecast();
  if (!f.valid) return 0;
  constexpr int16_t kIcon = 48, kGap = 10, kDeg = 7, kLine = 19, kDropW = 10;
  const weather::Day &today = f.days[0];
  gfx.setFont(&FreeSansBold9pt7b);
  const String now = tempText(f.now.temp, 0);
  const int16_t nowW = rawWidth(gfx, now) + kDeg;
  gfx.setFont(&FreeSans9pt7b);
  const String range = "  " + tempText(today.tmin, 0) + "/" + tempText(today.tmax, 0);
  const String rain = (today.rainChance < 0 ? String("--") : String(today.rainChance) + "%") + " " +
                      rainAmount(today.rain, f.imperial);
  const String sun = sunLine(f);
  const int16_t line1 = nowW + rawWidth(gfx, range) + kDeg;
  const int16_t textW = max(line1, max<int16_t>(kDropW + rawWidth(gfx, rain), rawWidth(gfx, sun)));
  const int16_t lines = sun.length() ? 3 : 2;
  const int16_t height = max<int16_t>(kIcon, lines * kLine);
  // Icon and text centred together, as one card.
  int16_t x = (kW - kIcon - kGap - textW) / 2;
  icons::drawWeather(gfx, f.now.code, f.now.isDay, x + kIcon / 2, height / 2, kIcon);
  x += kIcon + kGap;
  int16_t y = (height - lines * kLine) / 2 + 14;
  gfx.setFont(&FreeSansBold9pt7b);
  gfx.setCursor(x, y);
  gfx.print(now);
  gfx.drawCircle(x + nowW - 4, y - 11, 2, kBlack);  // degree sign
  gfx.setFont(&FreeSans9pt7b);
  gfx.setCursor(x + nowW, y);
  gfx.print(range);
  if (!isnan(today.tmax)) gfx.drawCircle(gfx.getCursorX() + 3, y - 11, 2, kBlack);
  y += kLine;
  drop(gfx, x + 3, y - 13);
  gfx.setCursor(x + kDropW, y);
  gfx.print(rain);
  if (sun.length()) {
    y += kLine;
    gfx.setCursor(x, y);
    gfx.print(sun);
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
      loc["source"] = s.source.length() ? s.source : String("city");
      loc["lat"] = s.lat;
      loc["lon"] = s.lon;
    }
    reply["units"] = s.imperial ? "imperial" : "metric";
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
  // {automatic: true} or {automatic: false, lat, lon, name, source: phone|city}
  ble::on("weather.location", [](JsonObjectConst args, JsonObject reply) {
    const bool automatic = args["automatic"] | true;
    const float lat = args["lat"] | NAN, lon = args["lon"] | NAN;
    if (!automatic && (isnan(lat) || isnan(lon))) {
      reply["ok"] = false;
      reply["error"] = "lat and lon are needed";
      return;
    }
    weather::setLocation(automatic, lat, lon, args["name"] | "", args["source"] | "city");
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
