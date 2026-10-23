// uses:
// The replay's recording kept compressed (cartridges/companion/adpcm_recorder.h): two minutes of a
// 24 kHz reply in ~1.4 MB instead of 5.8, decoded back when the WAV is written.
#include <cmath>
#include <vector>

#include "adpcm_recorder.h"
#include "check.h"

int main() {
  AdpcmRecorder r;
  r.reset(24000 * 3);  // room for 3 s
  std::vector<int16_t> pcm(24000 * 2);
  for (size_t i = 0; i < pcm.size(); i++) pcm[i] = static_cast<int16_t>(9000 * std::sin(i * 2 * M_PI * 300 / 24000.0));
  for (size_t at = 0; at < pcm.size(); at += 777) r.add(pcm.data() + at, std::min<size_t>(777, pcm.size() - at));
  CHECK_EQ("samples: all of them", r.samples(), pcm.size());
  CHECK("compressed: about a quarter", r.bytes() < pcm.size() * 2 / 3);
  std::vector<int16_t> out;
  r.decode([&](const int16_t *s, size_t n) { out.insert(out.end(), s, s + n); });
  CHECK_EQ("decode: every sample", out.size(), pcm.size());
  double err = 0, power = 0;
  for (size_t i = 0; i < pcm.size() && i < out.size(); i++) err += std::pow(out[i] - pcm[i], 2), power += std::pow(pcm[i], 2);
  CHECK("decode: close to the original (SNR > 20 dB)", 10 * std::log10(power / err) > 20);
  r.add(pcm.data(), pcm.size());  // 2 s more into room for 3 s
  CHECK_EQ("capped: the room it was given", r.samples(), size_t(24000 * 3));
  r.reset(100);
  CHECK("reset: empty", r.samples() == 0 && r.bytes() == 0);
  return failures ? 1 : 0;
}
