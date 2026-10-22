// PcmRing: the streaming playback buffer (lib/dotty_core/src/pcm_ring.h).
#include "check.h"
#include "pcm_ring.h"

int main() {
  int16_t storage[256];
  int16_t data[300];
  for (int i = 0; i < 300; i++) data[i] = static_cast<int16_t>(i);

  PcmRing ring(storage, 256);
  CHECK_EQ("ring: writing 300 into 256 takes 256", ring.write(data, 300), size_t(256));
  CHECK_EQ("ring: full has no space", ring.space(), size_t(0));

  int16_t out[100];
  CHECK_EQ("ring: read 100", ring.read(out, 100), size_t(100));
  CHECK("ring: reads in order", out[0] == 0 && out[99] == 99);
  CHECK_EQ("ring: write 100 after reading wraps", ring.write(data, 100), size_t(100));
  CHECK_EQ("ring: available after wrapping", ring.available(), size_t(256));

  int16_t all[256];
  ring.read(all, 256);
  CHECK("ring: wrapped data comes out in order", all[0] == 100 && all[155] == 255 && all[156] == 0 && all[255] == 99);

  PcmRing empty(storage, 256);
  CHECK_EQ("ring: reading from empty returns 0", empty.read(out, 10), size_t(0));

  ring.write(data, 50);
  ring.clear();
  CHECK_EQ("ring: clear empties", ring.available(), size_t(0));
  return failures ? 1 : 0;
}
