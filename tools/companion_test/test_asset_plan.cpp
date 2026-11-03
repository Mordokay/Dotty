// uses:
// When a pack on the card is the one the firmware wants, where it's downloaded from, and when to try.
#include <climits>
#include <string>

#include "asset_plan.h"
#include "check.h"

int main() {
  const AssetPack p = {"avatars", "companion-avatars-60225df3.bin", 252440,
                       "60225df3f2dda42824383bcbe5115d23e982f83ba5f160d8cd935767dc560594"};
  const std::string sha = p.sha256;
  CHECK("ready: the stamp is this pack", assetReady(sha, p));
  CHECK("ready: a newline after it is fine", assetReady(sha + "\n", p));
  CHECK("ready: no stamp", !assetReady("", p));
  CHECK("ready: another pack's stamp (an older firmware's)", !assetReady(std::string(64, 'a'), p));
  CHECK("ready: a cut stamp", !assetReady(sha.substr(0, 63), p));

  const std::string url = assetUrl(kReleaseAssetBase, p);
  CHECK_EQ("url: the latest release", url,
           std::string("https://github.com/Mordokay/Dotty/releases/latest/download/companion-avatars-60225df3.bin"));
  CHECK_EQ("url: a bench server", assetUrl("http://192.168.68.50:8000/", p),
           std::string("http://192.168.68.50:8000/companion-avatars-60225df3.bin"));

  AssetRetry r;
  CHECK("retry: not before 30 s", !r.isDue(29999));
  CHECK("retry: at 30 s", r.isDue(30000));
  r.failed(40000);
  CHECK_EQ("retry: 10 min after the first failure", r.due(), 640000u);
  r.failed(700000);
  CHECK_EQ("retry: an hour after the next", r.due(), 4300000u);
  r.failed(4400000);
  CHECK_EQ("retry: still an hour", r.due(), 8000000u);
  r.succeeded();
  CHECK("retry: done, never due", !r.isDue(UINT32_MAX));
  r.reset(5000);
  CHECK_EQ("retry: reset, 30 s later", r.due(), 35000u);
  r.failed(40000);
  CHECK_EQ("retry: a reset starts the 10 min step again", r.due(), 640000u);
  return failures ? 1 : 0;
}
