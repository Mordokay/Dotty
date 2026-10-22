#pragma once

#include <stddef.h>
#include <stdint.h>

class AudioPlayer;

// The companion's audio: push-to-talk capture in 100 ms chunks (a task on core 0 reads the
// microphone into a PSRAM stream buffer, so a slow network never makes it drop sound), the
// microphone level for the meter, and playback of a reply that arrives faster than it plays (a
// PSRAM backlog fed into the player's stream ring as it drains).
namespace voice {

constexpr uint32_t kInRate = 16000, kOutRate = 24000;
constexpr size_t kChunkFrames = 1600;  // 100 ms at 16 kHz

void begin(AudioPlayer &player);
bool startListening();
void stopListening();                        // stops the microphone; chunks already read stay queued
bool nextChunk(int16_t *out, size_t &frames);  // a queued chunk (up to kChunkFrames), false if none
int levelDb();                               // the last 100 ms peak, -60..0 dBFS

void playStart();
void play(const int16_t *pcm, size_t frames);  // queues (drops beyond ~90 s, logged)
void playEnd();                              // no more audio for this reply: play out the rest
void playStop();                             // cut it off now
bool playing();
size_t playedFrames();  // of this reply, as heard from the speaker
void poll();                                 // feeds the player from the backlog: call often

}  // namespace voice
