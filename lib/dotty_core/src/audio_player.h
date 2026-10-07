#pragma once

#include <Arduino.h>

// Streams an MP3 or WAV (16-bit PCM) file from the SD card to the speaker, plays short
// synthesised tunes (playNotes: chiptune-style beeps, no files), and captures the microphone. Decoding runs in its own FreeRTOS task, so slow e-paper refreshes on
// the main loop never interrupt playback.
class AudioPlayer {
 public:
  // Sets up the codec + amplifier and starts I2S at `sampleRate` (playback switches to
  // each file's rate; recording uses this one). The audio rail must be on and Wire running.
  bool begin(uint32_t sampleRate = 44100);

  // Codec + amplifier off/on around cutting the audio rail (codec registers are
  // lost without power). Playback position is kept; call while paused or stopped.
  void powerDown();
  bool powerUp();
  bool isPoweredUp() const { return !suspended_; }

  bool play(const char *path);  // .mp3 or .wav

  // A short tune, synthesised on the fly: square waves with a plucked envelope (a soft
  // piezo-like chirp). midi 0 = a rest. Replaces whatever was playing; isPlaying() until done.
  struct Note {
    uint8_t midi;
    uint16_t ms;
  };
  static constexpr size_t kMaxNotes = 48;
  bool playNotes(const Note *notes, size_t count);
  void stop();
  void togglePause();
  void setVolume(uint8_t percent);

  bool isPlaying() const { return playing_; }
  bool isPaused() const { return paused_; }
  uint8_t volume() const { return volume_; }
  uint32_t positionMs() const;
  uint32_t durationMs() const { return durationMs_; }

  // Microphone, at the rate given to begin() (stops playback; the speaker is off while
  // capturing). capture() blocks until `frames` mono samples have arrived.
  bool startCapture(uint8_t gainDb = 30, float digitalDb = 0);
  size_t capture(int16_t *mono, size_t frames);
  void stopCapture();
  bool isCapturing() const { return capturing_; }
  uint32_t captureRate() const { return captureRate_; }

 private:
  static void taskEntry(void *arg);
  void run();
  bool refill();
  void writeSilence();
  bool openWav();
  void playWav();
  void playSynth();

  TaskHandle_t task_ = nullptr;
  volatile bool playing_ = false;
  volatile bool paused_ = false;
  volatile bool stopRequested_ = false;
  volatile bool suspended_ = false;
  volatile bool capturing_ = false;
  volatile uint64_t samplesPlayed_ = 0;
  volatile uint32_t sampleRate_ = 44100;
  volatile uint32_t durationMs_ = 0;
  uint32_t captureRate_ = 44100;
  uint8_t volume_ = 70;
  // WAV: channels and PCM bytes still to play (0 for MP3).
  bool wav_ = false;
  uint8_t wavChannels_ = 1;
  uint32_t wavRemaining_ = 0;
  // Synthesised notes.
  bool synth_ = false;
  Note notes_[kMaxNotes];
  size_t noteCount_ = 0, noteIndex_ = 0;
  uint32_t noteSamples_ = 0, noteDone_ = 0, phase_ = 0, phaseStep_ = 0;
  uint8_t noteMidi_ = 0;
};
