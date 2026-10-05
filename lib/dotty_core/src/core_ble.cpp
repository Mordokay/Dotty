#include "core_ble.h"

#include <NimBLEDevice.h>

#include <map>
#include <string>

#include "battery.h"
#include "cartridge.h"
#include "log.h"

namespace ble {
namespace {

constexpr const char *kServiceUuid = "b9c10000-fbaa-4525-8400-055f7a543231";
constexpr const char *kInfoUuid = "b9c10001-fbaa-4525-8400-055f7a543231";
constexpr const char *kCommandUuid = "b9c10002-fbaa-4525-8400-055f7a543231";
constexpr const char *kEventUuid = "b9c10003-fbaa-4525-8400-055f7a543231";
constexpr const char *kDataUuid = "b9c10004-fbaa-4525-8400-055f7a543231";

constexpr size_t kMaxCommandLen = 512;
constexpr size_t kQueueDepth = 8;
constexpr uint32_t kInfoRefreshMs = 10000;
constexpr uint8_t kMoreFollows = 0x1E;

std::map<std::string, Handler> handlers;
InfoExtender infoExtender;
DataHandler dataHandler;
uint8_t serialBytes[6];
String serialText;
QueueHandle_t queue = nullptr;
NimBLECharacteristic *infoChar = nullptr;
NimBLECharacteristic *eventChar = nullptr;
volatile bool isConnected = false;
volatile uint16_t peerMtu = 23;
volatile bool pairingActive = false;
volatile uint32_t passkey = 0;
volatile bool pairingDone = false;
volatile bool pairingSucceeded = false;
bool isRunning = false;
String deviceName;

struct QueuedCommand {
  char json[kMaxCommandLen];
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
    isConnected = true;
    peerMtu = 23;
    LOGI("ble", "connected %s, interval %.2f ms", info.getAddress().toString().c_str(),
         info.getConnInterval() * 1.25f);
    // Faster transfers: 15-30 ms interval (Apple's accessory guidelines: min >= 15 ms,
    // max >= min + 15 ms) and 251-byte link-layer packets.
    server->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
    server->setDataLen(info.getConnHandle(), 251);
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
    isConnected = false;
    if (pairingActive) {
      pairingActive = false;
      pairingSucceeded = false;
      pairingDone = true;
    }
    LOGI("ble", "disconnected (reason %d)", reason);
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo &) override {
    peerMtu = mtu;
    LOGI("ble", "MTU %u", mtu);
  }
  void onConnParamsUpdate(NimBLEConnInfo &info) override {
    LOGI("ble", "interval %.2f ms, latency %u", info.getConnInterval() * 1.25f, info.getConnLatency());
  }
  // iOS asks for the code; Dotty shows a fresh random one on its screen.
  uint32_t onPassKeyDisplay() override {
    passkey = esp_random() % 1000000;
    pairingActive = true;
    LOGI("ble", "pairing: showing code");
    return passkey;
  }
  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    const bool ok = info.isEncrypted() && info.isAuthenticated();
    LOGI("ble", "pairing %s (bonded %d)", ok ? "succeeded" : "failed", info.isBonded());
    if (pairingActive || !ok) {
      pairingSucceeded = ok;
      pairingDone = true;
    }
    pairingActive = false;
    if (!ok) NimBLEDevice::getServer()->disconnect(info.getConnHandle());
  }
  void onPhyUpdate(NimBLEConnInfo &, uint8_t txPhy, uint8_t rxPhy) override {
    LOGI("ble", "PHY tx %u rx %u (2 = 2M)", txPhy, rxPhy);
  }
};

class CommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    const std::string value = c->getValue();
    if (value.empty() || value.size() >= kMaxCommandLen) {
      LOGW("ble", "command dropped (%u bytes)", static_cast<unsigned>(value.size()));
      return;
    }
    QueuedCommand cmd;
    memcpy(cmd.json, value.data(), value.size());
    cmd.json[value.size()] = '\0';
    if (xQueueSend(queue, &cmd, 0) != pdTRUE) LOGW("ble", "command queue full");
  }
};

class DataCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    if (!dataHandler) return;
    const NimBLEAttValue &value = c->getValue();
    dataHandler(value.data(), value.size());
  }
};

ServerCallbacks serverCallbacks;
CommandCallbacks commandCallbacks;
DataCallbacks dataCallbacks;

// The Info characteristic holds at most 512 bytes (a longer value is cut, and the app can't
// parse it), so it lists command namespaces ("features"); the core.info reply, which may be
// any length, also lists every command.
void buildInfo(JsonDocument &doc, bool withCommands) {
  const CartridgeInfo &me = cartridge::self();
  doc["role"] = cartridge::isLauncher() ? "launcher" : "cartridge";
  doc["id"] = me.id;
  doc["name"] = me.name;
  doc["version"] = me.version;
  doc["serial"] = serialText;
  doc["battery"] = battery::percent();
  doc["charging"] = battery::charging();
  doc["power"] = battery::external();
  JsonArray features = doc["features"].to<JsonArray>();
  String last;
  for (const auto &entry : handlers) {  // std::map: sorted, so namespaces come grouped
    const std::string &cmd = entry.first;
    const String ns = cmd.substr(0, cmd.find('.')).c_str();
    if (ns != last) features.add(ns);
    last = ns;
  }
  if (withCommands) {
    JsonArray cmds = doc["commands"].to<JsonArray>();
    for (const auto &entry : handlers) cmds.add(entry.first.c_str());
  }
  if (infoExtender) infoExtender(doc.as<JsonObject>());
}

void refreshInfo() {
  if (!infoChar) return;
  JsonDocument doc;
  buildInfo(doc, false);
  String json;
  serializeJson(doc, json);
  if (json.length() > 500) LOGW("ble", "Info is %u bytes; the limit is 512", static_cast<unsigned>(json.length()));
  infoChar->setValue(json.c_str());
}

void run(const char *json) {
  JsonDocument request;
  JsonDocument reply;
  if (deserializeJson(request, json) != DeserializationError::Ok || !request["cmd"].is<const char *>()) {
    LOGW("ble", "bad command: %s", json);
    reply["ok"] = false;
    reply["error"] = "bad json";
    notify(reply);
    return;
  }
  const char *name = request["cmd"];
  reply["cmd"] = name;
  const auto it = handlers.find(name);
  if (it == handlers.end()) {
    LOGW("ble", "unknown command %s", name);
    reply["ok"] = false;
    reply["error"] = "unknown command";
  } else {
    LOGI("ble", "command %s", name);
    it->second(request.as<JsonObjectConst>(), reply.as<JsonObject>());
    if (reply["ok"].isNull()) reply["ok"] = true;
  }
  notify(reply);
}

void registerCoreCommands() {
  on("core.ping", [](JsonObjectConst, JsonObject) {});
  on("core.info", [](JsonObjectConst, JsonObject reply) {
    JsonDocument info;
    buildInfo(info, true);
    reply["info"] = info;
  });
  // Forgets every bonded phone (after replying). The phone must also "Forget This
  // Device" in its Bluetooth settings.
  on("core.forget", [](JsonObjectConst, JsonObject) {
    static TimerHandle_t timer = xTimerCreate(
        "forget", pdMS_TO_TICKS(300), pdFALSE, nullptr, [](TimerHandle_t) {
          NimBLEDevice::deleteAllBonds();
          LOGI("ble", "all bonds deleted");
        });
    xTimerStart(timer, 0);
  });
  if (!cartridge::isLauncher()) {
    on("core.toLauncher", [](JsonObjectConst, JsonObject) {
      // Reply first, then reboot once the event has had time to go out.
      static TimerHandle_t timer = xTimerCreate(
          "toLauncher", pdMS_TO_TICKS(300), pdFALSE, nullptr,
          [](TimerHandle_t) { cartridge::rebootToLauncher(); });
      xTimerStart(timer, 0);
    });
  }
}

}  // namespace

void begin() {
  queue = xQueueCreate(kQueueDepth, sizeof(QueuedCommand));
  const uint64_t mac = ESP.getEfuseMac();
  for (int i = 0; i < 6; i++) serialBytes[i] = static_cast<uint8_t>(mac >> (8 * i));
  char text[18];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", serialBytes[0], serialBytes[1],
           serialBytes[2], serialBytes[3], serialBytes[4], serialBytes[5]);
  serialText = text;
  char name[16];
  snprintf(name, sizeof(name), "Dotty-%02X%02X", serialBytes[4], serialBytes[5]);
  deviceName = name;
  registerCoreCommands();
  start();
}

void start() {
  if (isRunning) return;
  NimBLEDevice::init(deviceName.c_str());
  NimBLEDevice::setMTU(517);
  // Bonded, MITM-protected (passkey), LE Secure Connections; Dotty can only display.
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  // Prefer the 2M PHY (double bitrate) when the phone/Mac supports it.
  NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks, false);
  server->advertiseOnDisconnect(true);

  NimBLEService *service = server->createService(kServiceUuid);
  constexpr uint32_t kSecureWrite = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
                                     NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN;
  infoChar = service->createCharacteristic(kInfoUuid, NIMBLE_PROPERTY::READ);
  service->createCharacteristic(kCommandUuid, kSecureWrite)->setCallbacks(&commandCallbacks);
  eventChar = service->createCharacteristic(
      kEventUuid, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
                      NIMBLE_PROPERTY::READ_AUTHEN);
  service->createCharacteristic(kDataUuid, kSecureWrite)->setCallbacks(&dataCallbacks);
  refreshInfo();

  // Advertisement (31 bytes, full): flags + service UUID + manufacturer data with the
  // serial. The name goes in the scan response.
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.addServiceUUID(kServiceUuid);
  uint8_t manufacturer[8] = {0xFF, 0xFF};  // 0xFFFF: no registered company id
  memcpy(manufacturer + 2, serialBytes, sizeof(serialBytes));
  advData.setManufacturerData(manufacturer, sizeof(manufacturer));
  NimBLEAdvertisementData scanData;
  scanData.setName(deviceName.c_str());

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->start();
  isRunning = true;
  LOGI("ble", "advertising as %s", deviceName.c_str());
}

void stop() {
  if (!isRunning) return;
  NimBLEDevice::deinit(true);
  infoChar = nullptr;
  eventChar = nullptr;
  isConnected = false;
  isRunning = false;
  LOGI("ble", "off");
}

bool running() {
  return isRunning;
}

void on(const char *cmd, Handler handler) {
  handlers[cmd] = std::move(handler);
}

void extendInfo(InfoExtender extender) {
  infoExtender = std::move(extender);
}

void onData(DataHandler handler) {
  dataHandler = std::move(handler);
}

const String &serial() {
  return serialText;
}

int poll() {
  if (!isRunning) return 0;
  int ran = 0;
  QueuedCommand cmd;
  while (xQueueReceive(queue, &cmd, 0) == pdTRUE) {
    run(cmd.json);
    ran++;
  }

  static uint32_t lastInfo = 0;
  if (millis() - lastInfo >= kInfoRefreshMs) {
    lastInfo = millis();
    refreshInfo();
  }
  return ran;
}

// Messages longer than one notification go out in pieces: every piece but the last starts
// with kMoreFollows (0x1E), and the client appends pieces until one doesn't.
void notify(JsonDocument &event) {
  if (!isConnected || !eventChar) return;
  String json;
  serializeJson(event, json);
  const size_t room = peerMtu > 3 ? peerMtu - 3 : 20;
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(json.c_str());
  size_t left = json.length();
  if (left <= room) {
    eventChar->notify(bytes, left);
    return;
  }
  uint8_t piece[517];
  while (left > 0 && isConnected) {
    const bool last = left <= room;
    size_t n = 0;
    if (!last) piece[n++] = kMoreFollows;
    const size_t take = last ? left : min(left, room - 1);
    memcpy(piece + n, bytes, take);
    n += take;
    // The stack refuses when its buffers are full; give it a moment and retry.
    bool sent = false;
    for (int tries = 0; tries < 50 && !(sent = eventChar->notify(piece, n)); tries++) delay(5);
    if (!sent) {
      LOGW("ble", "notify dropped (%u bytes left)", static_cast<unsigned>(left));
      return;
    }
    bytes += take;
    left -= take;
  }
}

bool connected() {
  return isConnected;
}

bool pairingCode(uint32_t &code) {
  code = passkey;
  return pairingActive;
}

bool takePairingResult(bool &success) {
  if (!pairingDone) return false;
  pairingDone = false;
  success = pairingSucceeded;
  return true;
}

}  // namespace ble
