// uses:
// The voice samples built into the firmware: one file, "DVS1", a count, then
// {name[16], offset, size} per sample (little-endian), then the MP3s.
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "voice_pack.h"

static void put32(std::vector<uint8_t> &b, uint32_t v) {
  for (int i = 0; i < 4; i++) b.push_back(v >> (8 * i));
}

int main() {
  std::vector<uint8_t> pack = {'D', 'V', 'S', '1'};
  put32(pack, 2);
  const char *names[] = {"Zephyr", "Achernar"};
  const std::string data[] = {"abc", "hello"};
  uint32_t offset = 8 + 2 * 24;
  for (int i = 0; i < 2; i++) {
    char name[16] = {};
    strncpy(name, names[i], 15);
    pack.insert(pack.end(), name, name + 16);
    put32(pack, offset);
    put32(pack, data[i].size());
    offset += data[i].size();
  }
  for (const std::string &d : data) pack.insert(pack.end(), d.begin(), d.end());

  VoicePack p(pack.data(), pack.size());
  CHECK("pack: valid", p.valid());
  CHECK_EQ("pack: count", p.count(), 2);
  CHECK_EQ("pack: name", std::string(p.name(1)), std::string("Achernar"));
  const uint8_t *bytes = nullptr;
  size_t len = 0;
  CHECK("pack: finds a sample", p.find("Achernar", bytes, len) && len == 5 && memcmp(bytes, "hello", 5) == 0);
  CHECK("pack: unknown voice", !p.find("Kore", bytes, len));
  VoicePack bad(pack.data(), 30);  // cut short: the index runs past the end
  CHECK("pack: a cut file is invalid", !bad.valid());
  pack[0] = 'X';
  CHECK("pack: wrong magic", !VoicePack(pack.data(), pack.size()).valid());

  // The real pack (tools/build_voice_pack.py), as the Companion downloads it to the card.
  std::ifstream in("../../cartridges/companion/assets/voices.bin", std::ios::binary);
  const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const VoicePack real(file.data(), file.size());
  CHECK("voice pack file: valid", !file.empty() && real.valid());
  CHECK_EQ("voice pack file: 30 samples", real.valid() ? real.count() : 0, 30);
  CHECK("voice pack file: has Zephyr", real.valid() && real.find("Zephyr", bytes, len) && len > 0);
  return failures ? 1 : 0;
}
