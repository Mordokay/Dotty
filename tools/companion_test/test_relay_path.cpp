// uses:
// Which way a chat goes (cartridges/companion/relay_path.h): the phone over Bluetooth, or Wi-Fi raced
// against it; and the grace window that lets the phone take over a Wi-Fi chat.
#include "check.h"
#include "relay_path.h"

int main() {
  RelayPath p;
  p.press(1000, true);
  CHECK("phone ready at the press: relay, no race", p.way() == RelayPath::Relay && !p.racing());
  p.end();
  CHECK("end: none", p.way() == RelayPath::None);

  p.press(1000, false);
  CHECK("no phone: racing", p.racing() && p.way() == RelayPath::None && p.keepBluetooth(1000));
  p.phoneReady(2500);
  CHECK("phone first: relay", p.way() == RelayPath::Relay && !p.racing());
  CHECK("phone first: drop the Wi-Fi join, once", p.takeDropWifi() && !p.takeDropWifi());
  p.wifiReady(6000);
  CHECK("Wi-Fi after the phone won: ignored", p.way() == RelayPath::Relay);
  p.phoneLost();
  CHECK("the phone lost in a relay chat: none (the next press chooses again)", p.way() == RelayPath::None);
  p.end();

  p.press(0, false);
  p.wifiReady(7000);
  CHECK("Wi-Fi first: Wi-Fi", p.way() == RelayPath::Wifi && !p.racing() && !p.takeDropWifi());
  CHECK("window: on while the first reply hasn't ended", p.keepBluetooth(7000 + 60000));
  p.replyEnded(7000 + 5000);
  CHECK("window: at least 15 s", p.keepBluetooth(7000 + 14000));
  CHECK("window: then off (the reply ended at +5 s)", !p.keepBluetooth(7000 + 16000));
  p.end();

  p.press(0, false);
  p.wifiReady(7000);
  p.replyEnded(7000 + 20000);
  CHECK("window: until the first reply ends when that's later", p.keepBluetooth(7000 + 20000) && !p.keepBluetooth(7000 + 21000));
  p.replyEnded(7000 + 40000);
  CHECK("window: only the first reply counts", !p.keepBluetooth(7000 + 30000));
  p.end();

  p.press(0, false);
  p.wifiReady(7000);
  p.phoneReady(9000);  // mid-reply, inside the window
  CHECK("phone in the window: no switch mid-turn", p.way() == RelayPath::Wifi);
  CHECK("phone in the window: taken at the next press, once", p.takeHandover() && p.way() == RelayPath::Relay && !p.takeHandover());
  p.end();

  p.press(0, false);
  p.wifiReady(7000);
  p.replyEnded(8000);
  p.phoneReady(7000 + 30000);
  CHECK("phone after the window: no hand-over", !p.takeHandover() && p.way() == RelayPath::Wifi);
  return failures ? 1 : 0;
}
