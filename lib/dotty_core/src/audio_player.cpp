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

bool AudioPlayer::begin(uint32_t sampleRate) {
  pinMode(PIN_PA_EN, OUTPUT);
  digitalWrite(PIN_PA_EN, LOW);
  delay(50);

  // Start I2S first so the codec sees MCLK while it is configured. TX and RX share the
  // port's clock, so recording happens at this rate.
  sampleRate_ = captureRate_ = sampleRate;
  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, sampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    LOGE("audio", "I2S init failed");
    return false;
  }
  if (!codec.begin(Wire, I2C_ADDR_ES8311, sampleRate)) return false;
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
  if (capturing_) return false;
  file = SD_MMC.open(path);
  if (!file) {
    LOGE("audio", "Cannot open %s", path);
    return false;
  }
  String lower = path;
  lower.toLowerCase();
  synth_ = false;
  wav_ = lower.endsWith(".wav");
  if (wav_) {
    if (!openWav()) {
      LOGE("audio", "%s: not a 16-bit PCM WAV", path);
      file.close();
      return false;
    }
    samplesPlayed_ = 0;
    codec.setMute(false);
    paused_ = false;
    stopRequested_ = false;
    playing_ = true;
    return true;
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

bool AudioPlayer::playNotes(const Note *notes, size_t count) {
  stop();
  if (capturing_ || suspended_ || count == 0) return false;
  noteCount_ = min(count, kMaxNotes);
  memcpy(notes_, notes, noteCount_ * sizeof(Note));
  noteIndex_ = 0;
  noteSamples_ = noteDone_ = 0;
  synth_ = true;
  wav_ = false;
  codec.setMute(false);
  paused_ = false;
  stopRequested_ = false;
  playing_ = true;
  return true;
}

// One block of the current tune: square waves at the note's pitch, plucked (a quick attack,
// then fading to a third) with a short release so notes don't click.
void AudioPlayer::playSynth() {
  constexpr size_t kFrames = 256;
  constexpr int32_t kAmplitude = 6000;
  const uint32_t rate = sampleRate_;
  const uint32_t attack = rate * 3 / 1000, release = rate * 8 / 1000;
  for (size_t i = 0; i < kFrames; i++) {
    if (noteDone_ >= noteSamples_) {
      if (noteIndex_ >= noteCount_) {
        if (i) i2s.write(reinterpret_cast<uint8_t *>(pcm), i * 2 * sizeof(int16_t));
        synth_ = false;
        playing_ = false;
        return;
      }
      const Note &n = notes_[noteIndex_++];
      noteMidi_ = n.midi;
      noteSamples_ = static_cast<uint32_t>(n.ms) * rate / 1000;
      noteDone_ = 0;
      const float freq = 440.0f * powf(2.0f, (static_cast<int>(n.midi) - 69) / 12.0f);
      phaseStep_ = static_cast<uint32_t>(freq / rate * 4294967296.0f);
    }
    int32_t v = 0;
    if (noteMidi_) {
      const float progress = static_cast<float>(noteDone_) / noteSamples_;
      float env = 1.0f - 0.66f * progress;
      if (noteDone_ < attack) env *= static_cast<float>(noteDone_) / attack;
      const uint32_t left = noteSamples_ - noteDone_;
      if (left < release) env *= static_cast<float>(left) / release;
      v = static_cast<int32_t>((phase_ & 0x80000000u ? kAmplitude : -kAmplitude) * env);
      phase_ += phaseStep_;
    }
    pcm[2 * i] = pcm[2 * i + 1] = static_cast<int16_t>(v);
    noteDone_++;
  }
  i2s.write(reinterpret_cast<uint8_t *>(pcm), kFrames * 2 * sizeof(int16_t));
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

// RIFF/WAVE: walks the chunks to "fmt " (PCM, 16-bit, 1-2 channels) and "data", and
// leaves the file at the first sample.
bool AudioPlayer::openWav() {
  uint8_t riff[12];
  if (file.read(riff, 12) != 12 || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) return false;
  uint32_t rate = 0;
  uint16_t channels = 0, bits = 0, format = 0;
  for (;;) {
    uint8_t header[8];
    if (file.read(header, 8) != 8) return false;
    uint32_t size;
    memcpy(&size, header + 4, 4);
    if (memcmp(header, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      if (size < 16 || file.read(fmt, 16) != 16) return false;
      memcpy(&format, fmt, 2);
      memcpy(&channels, fmt + 2, 2);
      memcpy(&rate, fmt + 4, 4);
      memcpy(&bits, fmt + 14, 2);
      file.seek(file.position() + size - 16 + (size & 1));
    } else if (memcmp(header, "data", 4) == 0) {
      if (format != 1 || bits != 16 || channels < 1 || channels > 2 || rate == 0) return false;
      // A recording that never got its final header (power cut) says 0 or too much: use the file.
      const uint32_t available = file.size() - file.position();
      wavRemaining_ = size == 0 || size > available ? available : size;
      wavChannels_ = channels;
      durationMs_ = static_cast<uint64_t>(wavRemaining_) * 1000 / (rate * channels * 2);
      if (rate != sampleRate_) {
        sampleRate_ = rate;
        i2s.configureTX(rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
        codec.setSampleRate(rate);
      }
      return true;
    } else {
      file.seek(file.position() + size + (size & 1));
    }
  }
}

// One block of WAV samples to the speaker (mono doubled to both sides).
void AudioPlayer::playWav() {
  const size_t frameBytes = wavChannels_ * 2;
  const size_t want = min<size_t>(wavRemaining_, 512 * frameBytes);
  const size_t got = want ? file.read(reinterpret_cast<uint8_t *>(pcm), want) : 0;
  const size_t frames = got / frameBytes;
  if (frames == 0) {
    file.close();
    playing_ = false;  // end of file
    return;
  }
  wavRemaining_ -= frames * frameBytes;
  if (wavChannels_ == 1) {
    for (int i = frames - 1; i >= 0; i--) pcm[2 * i] = pcm[2 * i + 1] = pcm[i];
  }
  i2s.write(reinterpret_cast<uint8_t *>(pcm), frames * 2 * sizeof(int16_t));
  samplesPlayed_ += frames;
}

bool AudioPlayer::startCapture(uint8_t gainDb, float digitalDb) {
  stop();
  if (sampleRate_ != captureRate_) {  // a WAV at another rate played last
    sampleRate_ = captureRate_;
    i2s.configureTX(captureRate_, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    codec.setSampleRate(captureRate_);
  }
  digitalWrite(PIN_PA_EN, LOW);  // the amplifier only adds hiss to the microphone
  codec.setMicrophone(true, gainDb, digitalDb);
  // Drop what the DMA buffers held from before.
  static int16_t discard[256 * 2];
  for (int i = 0; i < 8; i++) i2s.readBytes(reinterpret_cast<char *>(discard), sizeof(discard));
  capturing_ = true;
  return true;
}

// The microphone arrives on the left slot of the stereo frames.
size_t AudioPlayer::capture(int16_t *mono, size_t frames) {
  static int16_t stereo[256 * 2];
  size_t done = 0;
  while (capturing_ && done < frames) {
    const size_t want = min<size_t>(frames - done, 256);
    const size_t got = i2s.readBytes(reinterpret_cast<char *>(stereo), want * 4) / 4;
    if (got == 0) break;
    for (size_t i = 0; i < got; i++) mono[done + i] = stereo[2 * i];
    done += got;
  }
  return done;
}

void AudioPlayer::stopCapture() {
  if (!capturing_) return;
  capturing_ = false;
  codec.setMicrophone(false);
  if (!suspended_) digitalWrite(PIN_PA_EN, HIGH);
}

void AudioPlayer::taskEntry(void *arg) {
  static_cast<AudioPlayer *>(arg)->run();
}

void AudioPlayer::run() {
  for (;;) {
    if (playing_ && stopRequested_) {
      synth_ = false;
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
    if (synth_) {
      playSynth();
      continue;
    }
    if (wav_) {
      playWav();
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
