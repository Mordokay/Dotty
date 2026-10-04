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

std::map<std::string, Handler> handlers;
InfoExtender infoExtender;
QueueHandle_t queue = nullptr;
NimBLECharacteristic *infoChar = nullptr;
NimBLECharacteristic *eventChar = nullptr;
volatile bool isConnected = false;
bool isRunning = false;
String deviceName;

struct QueuedCommand {
  char json[kMaxCommandLen];
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &info) override {
    isConnected = true;
    LOGI("ble", "connected %s", info.getAddress().toString().c_str());
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
    isConnected = false;
    LOGI("ble", "disconnected (reason %d)", reason);
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo &) override {
    LOGI("ble", "MTU %u", mtu);
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

ServerCallbacks serverCallbacks;
CommandCallbacks commandCallbacks;

void buildInfo(JsonDocument &doc) {
  const CartridgeInfo &me = cartridge::self();
  doc["role"] = cartridge::isLauncher() ? "launcher" : "cartridge";
  doc["id"] = me.id;
  doc["name"] = me.name;
  doc["version"] = me.version;
  doc["battery"] = batteryPercent(batteryMillivolts());
  JsonArray cmds = doc["commands"].to<JsonArray>();
  for (const auto &entry : handlers) cmds.add(entry.first.c_str());
  if (infoExtender) infoExtender(doc.as<JsonObject>());
}

void refreshInfo() {
  if (!infoChar) return;
  JsonDocument doc;
  buildInfo(doc);
  String json;
  serializeJson(doc, json);
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
    buildInfo(info);
    reply["info"] = info;
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
  char name[16];
  snprintf(name, sizeof(name), "Dotty-%02X%02X", static_cast<uint8_t>(mac >> 32),
           static_cast<uint8_t>(mac >> 40));
  deviceName = name;
  registerCoreCommands();
  start();
}

void start() {
  if (isRunning) return;
  NimBLEDevice::init(deviceName.c_str());
  NimBLEDevice::setMTU(517);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks, false);
  server->advertiseOnDisconnect(true);

  NimBLEService *service = server->createService(kServiceUuid);
  infoChar = service->createCharacteristic(kInfoUuid, NIMBLE_PROPERTY::READ);
  NimBLECharacteristic *command =
      service->createCharacteristic(kCommandUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  command->setCallbacks(&commandCallbacks);
  eventChar = service->createCharacteristic(kEventUuid, NIMBLE_PROPERTY::NOTIFY);
  service->createCharacteristic(kDataUuid, NIMBLE_PROPERTY::WRITE_NR);
  refreshInfo();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName(deviceName.c_str());
  adv->addServiceUUID(kServiceUuid);
  adv->enableScanResponse(true);
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

void poll() {
  if (!isRunning) return;
  QueuedCommand cmd;
  while (xQueueReceive(queue, &cmd, 0) == pdTRUE) run(cmd.json);

  static uint32_t lastInfo = 0;
  if (millis() - lastInfo >= kInfoRefreshMs) {
    lastInfo = millis();
    refreshInfo();
  }
}

void notify(JsonDocument &event) {
  if (!isConnected || !eventChar) return;
  String json;
  serializeJson(event, json);
  eventChar->setValue(json.c_str());
  eventChar->notify();
}

bool connected() {
  return isConnected;
}

}  // namespace ble
