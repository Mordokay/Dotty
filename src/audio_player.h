#pragma once

#include <Arduino.h>

// Streams an MP3 file from the SD card to the speaker.
// Decoding runs in its own FreeRTOS task, so slow e-paper refreshes on the
// main loop never interrupt playback.
class AudioPlayer {
 public:
  // Powers the codec + amplifier and starts I2S. Wire must already be running.
  bool begin();

  bool play(const char *path);
  void stop();
  void togglePause();
  void setVolume(uint8_t percent);

  bool isPlaying() const { return playing_; }
  bool isPaused() const { return paused_; }
  uint8_t volume() const { return volume_; }
  uint32_t positionMs() const;
  uint32_t durationMs() const { return durationMs_; }

 private:
  static void taskEntry(void *arg);
  void run();
  bool refill();
  void writeSilence();

  TaskHandle_t task_ = nullptr;
  volatile bool playing_ = false;
  volatile bool paused_ = false;
  volatile bool stopRequested_ = false;
  volatile uint64_t samplesPlayed_ = 0;
  volatile uint32_t sampleRate_ = 44100;
  volatile uint32_t durationMs_ = 0;
  uint8_t volume_ = 70;
};
