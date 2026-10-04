#include "audio_player.h"

#include <ESP_I2S.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <mp3dec.h>

#include "board_pins.h"
#include "es8311.h"
#include "log.h"

namespace {

constexpr size_t kInputBufferLen = 8 * 1024;
constexpr uint32_t kDefaultSampleRate = 44100;

I2SClass i2s;
Es8311 codec;
File file;
HMP3Decoder decoder = nullptr;

uint8_t inputBuffer[kInputBufferLen];
uint8_t *readPtr = inputBuffer;
int bytesLeft = 0;
int16_t pcm[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];

// Returns the size of an ID3v2 tag at the start of the file, 0 if none.
size_t id3v2Size(File &f) {
  uint8_t header[10];
  if (f.read(header, sizeof(header)) != sizeof(header) || memcmp(header, "ID3", 3) != 0) {
    f.seek(0);
    return 0;
  }
  // Tag size is "syncsafe": 4 bytes of 7 bits each.
  const size_t size = (header[6] & 0x7F) << 21 | (header[7] & 0x7F) << 14 |
                      (header[8] & 0x7F) << 7 | (header[9] & 0x7F);
  const size_t total = 10 + size + ((header[5] & 0x10) ? 10 : 0);
  f.seek(total);
  return total;
}

}  // namespace

bool AudioPlayer::begin() {
  pinMode(PIN_PA_EN, OUTPUT);
  digitalWrite(PIN_PA_EN, LOW);
  delay(50);

  // Start I2S first so the codec sees MCLK while it is configured.
  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, kDefaultSampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    LOGE("audio", "I2S init failed");
    return false;
  }
  if (!codec.begin(Wire, I2C_ADDR_ES8311, kDefaultSampleRate)) return false;
  codec.setVolume(volume_);
  digitalWrite(PIN_PA_EN, HIGH);

  decoder = MP3InitDecoder();
  if (!decoder) {
    LOGE("audio", "MP3 decoder alloc failed");
    return false;
  }
  xTaskCreatePinnedToCore(taskEntry, "audio", 8192, this, configMAX_PRIORITIES - 2, &task_, 0);
  return true;
}

bool AudioPlayer::play(const char *path) {
  stop();
  file = SD_MMC.open(path);
  if (!file) {
    LOGE("audio", "Cannot open %s", path);
    return false;
  }
  const size_t audioBytes = file.size() - id3v2Size(file);

  // Read the first frame header for sample rate + bitrate (constant-bitrate estimate).
  bytesLeft = 0;
  readPtr = inputBuffer;
  refill();
  MP3FrameInfo info = {};
  const int sync = MP3FindSyncWord(inputBuffer, bytesLeft);
  if (sync < 0 || MP3GetNextFrameInfo(decoder, &info, inputBuffer + sync) != 0) {
    LOGE("audio", "No MP3 frames in %s", path);
    file.close();
    return false;
  }
  durationMs_ = info.bitrate > 0 ? static_cast<uint64_t>(audioBytes) * 8000 / info.bitrate : 0;
  LOGI("audio", "%s: %d Hz, %d ch, %d kbps, ~%lu s", path, info.samprate, info.nChans,
        info.bitrate / 1000, durationMs_ / 1000);

  samplesPlayed_ = 0;
  codec.setMute(false);
  paused_ = false;
  stopRequested_ = false;
  playing_ = true;
  return true;
}

void AudioPlayer::stop() {
  if (!playing_) return;
  stopRequested_ = true;
  while (playing_) delay(5);
}

void AudioPlayer::powerDown() {
  if (suspended_) return;
  suspended_ = true;
  delay(20);  // let the task finish its current write
  digitalWrite(PIN_PA_EN, LOW);
}

bool AudioPlayer::powerUp() {
  if (!suspended_) return true;
  if (!codec.begin(Wire, I2C_ADDR_ES8311, sampleRate_)) return false;
  codec.setVolume(volume_);
  codec.setMute(paused_);
  digitalWrite(PIN_PA_EN, HIGH);
  suspended_ = false;
  return true;
}

void AudioPlayer::togglePause() {
  if (!playing_) return;
  paused_ = !paused_;
  codec.setMute(paused_);
}

void AudioPlayer::setVolume(uint8_t percent) {
  volume_ = min<uint8_t>(percent, 100);
  codec.setVolume(volume_);
}

uint32_t AudioPlayer::positionMs() const {
  return sampleRate_ ? samplesPlayed_ * 1000 / sampleRate_ : 0;
}

void AudioPlayer::taskEntry(void *arg) {
  static_cast<AudioPlayer *>(arg)->run();
}

void AudioPlayer::run() {
  for (;;) {
    if (playing_ && stopRequested_) {
      file.close();
      paused_ = false;
      playing_ = false;
      continue;
    }
    if (suspended_) {
      delay(50);
      continue;
    }
    if (!playing_ || paused_) {
      writeSilence();  // keeps I2S clocked and the DMA free of stale samples
      continue;
    }

    if (bytesLeft < MAINBUF_SIZE && !refill() && bytesLeft == 0) {
      file.close();
      playing_ = false;  // end of file
      continue;
    }
    const int sync = MP3FindSyncWord(readPtr, bytesLeft);
    if (sync < 0) {
      bytesLeft = 0;
      continue;
    }
    readPtr += sync;
    bytesLeft -= sync;

    const int err = MP3Decode(decoder, &readPtr, &bytesLeft, pcm, 0);
    if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
      if (!refill()) bytesLeft = 0;
      continue;
    }
    if (err != ERR_MP3_NONE) {
      // Corrupt frame: skip a byte and resync.
      readPtr++;
      bytesLeft--;
      continue;
    }

    MP3FrameInfo info;
    MP3GetLastFrameInfo(decoder, &info);
    if (static_cast<uint32_t>(info.samprate) != sampleRate_) {
      sampleRate_ = info.samprate;
      i2s.configureTX(sampleRate_, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
      codec.setSampleRate(sampleRate_);
    }

    size_t frames = info.outputSamps / info.nChans;
    if (info.nChans == 1) {
      // Mono: duplicate each sample to left + right, working backwards in place.
      for (int i = frames - 1; i >= 0; i--) {
        pcm[2 * i] = pcm[2 * i + 1] = pcm[i];
      }
    }
    i2s.write(reinterpret_cast<uint8_t *>(pcm), frames * 2 * sizeof(int16_t));
    samplesPlayed_ += frames;
  }
}

// Moves unread bytes to the front of the buffer and tops it up from the file.
bool AudioPlayer::refill() {
  if (readPtr != inputBuffer && bytesLeft > 0) memmove(inputBuffer, readPtr, bytesLeft);
  readPtr = inputBuffer;
  if (!file || !file.available()) return false;
  const size_t n = file.read(inputBuffer + bytesLeft, kInputBufferLen - bytesLeft);
  bytesLeft += n;
  return n > 0;
}

void AudioPlayer::writeSilence() {
  static const int16_t silence[256 * 2] = {};
  i2s.write(reinterpret_cast<const uint8_t *>(silence), sizeof(silence));
}
