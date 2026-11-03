// uses:
// The avatars on the card (all but the robot): "DAV1", a count, id[16] per avatar, then per avatar
// 12 faces at 128 px (2048 B) and 12 at 48 px (288 B).
#include <cstring>
#include <string>
#include <vector>

#include "avatar_pack.h"
#include "check.h"

static void put32(std::vector<uint8_t> &b, uint32_t v) {
  for (int i = 0; i < 4; i++) b.push_back(v >> (8 * i));
}

// Each face starts with avatar index * 16 + face.
static std::vector<uint8_t> makePack(const std::vector<std::string> &ids, const char *magic = "DAV1") {
  std::vector<uint8_t> b(magic, magic + 4);
  put32(b, ids.size());
  for (const std::string &id : ids) {
    char name[16] = {};
    strncpy(name, id.c_str(), 15);
    b.insert(b.end(), name, name + 16);
  }
  for (size_t a = 0; a < ids.size(); a++) {
    for (int f = 0; f < 12; f++) {
      std::vector<uint8_t> face(2048, 0);
      face[0] = a * 16 + f;
      b.insert(b.end(), face.begin(), face.end());
    }
    for (int f = 0; f < 12; f++) {
      std::vector<uint8_t> face(288, 0);
      face[0] = a * 16 + f;
      b.insert(b.end(), face.begin(), face.end());
    }
  }
  return b;
}

int main() {
  const std::vector<uint8_t> buf = makePack({"dog", "cat"});
  AvatarPack p(buf.data(), buf.size());
  CHECK("avatar pack: valid", p.valid());
  CHECK_EQ("avatar pack: count", p.count(), 2);
  CHECK_EQ("avatar pack: id", std::string(p.id(1)), std::string("cat"));
  const uint8_t *big = p.face("cat", 3, true), *small = p.face("cat", 3, false);
  CHECK("avatar pack: big face found", big && big[0] == 0x13);
  CHECK_EQ("avatar pack: big face offset", (long)(big - buf.data()), (long)(8 + 32 + 28032 + 3 * 2048));
  CHECK("avatar pack: small face found", small && small[0] == 0x13);
  CHECK_EQ("avatar pack: small face offset", (long)(small - buf.data()), (long)(8 + 32 + 28032 + 24576 + 3 * 288));
  CHECK("avatar pack: first avatar's first face", p.face("dog", 0, true) == buf.data() + 8 + 32);
  CHECK("avatar pack: the robot isn't in it", p.face("robot", 0, true) == nullptr);
  CHECK("avatar pack: unknown id", p.face("nope", 0, false) == nullptr);
  CHECK("avatar pack: face out of range", p.face("dog", 12, true) == nullptr && p.face("dog", -1, true) == nullptr);

  AvatarPack shortPack(buf.data(), buf.size() - 1);
  CHECK("avatar pack: one byte short is invalid", !shortPack.valid() && shortPack.face("dog", 0, true) == nullptr);
  const std::vector<uint8_t> wrong = makePack({"dog"}, "DAV2");
  AvatarPack wrongMagic(wrong.data(), wrong.size());
  CHECK("avatar pack: wrong magic is invalid", !wrongMagic.valid() && wrongMagic.face("dog", 0, true) == nullptr);
  AvatarPack empty(nullptr, 0);
  CHECK("avatar pack: no data is invalid", !empty.valid() && empty.face("dog", 0, true) == nullptr);
  return failures ? 1 : 0;
}
