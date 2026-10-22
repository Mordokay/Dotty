#pragma once

#include <Arduino.h>

#include <vector>

// What the companion's screen shows (main.cpp decides, screens draws).
enum class Talk { Offline, Connecting, Ready, Listening, Thinking, Speaking, After, Error };

struct ScreenInfo {
  Talk state = Talk::Offline;
  String status;         // one line under the avatar
  String heard, said;    // this turn's words
  String mood = "neutral";
  String avatar = "robot";  // the avatar id (settings)
  int levelDb = -60;     // the microphone meter while listening
  bool textOnly = false;
  int frame = 0;         // the animation clock (blink, bob, talking mouth)
  int page = -1;         // a long reply's page after it ends (-1 = the last one)
};

struct SettingsView {
  int volume = 70;
  bool speak = true;
  String voice, style;
  int choices = 2;  // favourite voices to step through (no arrows for one)
};
enum class SettingsHit { None, Back, VolumeDown, VolumeUp, Speak, VoicePrev, VoiceNext };

namespace screens {

constexpr int16_t kAvatarSize = 96;
void draw(const ScreenInfo &info);
// The last exchanges (top-right corner): "you" / "Dotty" pairs, newest last.
void drawHistory(const std::vector<std::pair<String, String>> &exchanges, int scroll);
// Settings (top-left gear): volume, out loud / text only, the voice.
void drawSettings(const SettingsView &v);
SettingsHit settingsHit(uint16_t x, uint16_t y);
// Pages a reply takes (for swiping through it).
int replyPages(const ScreenInfo &info);

}  // namespace screens
