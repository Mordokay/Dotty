#include "cartridge.h"

#include <esp_app_desc.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "log.h"

extern "C" const CartridgeInfo kDottyCartridge;  // defined by DOTTY_CARTRIDGE()

// The bootloader's rollback is on (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): a firmware
// started for the first time is "pending verify" until it confirms itself. Arduino would
// confirm at once; deferring lets a firmware that crashes early be rolled back (the shell
// confirms after a few seconds of running, see cartridge::confirmHealthy).
extern "C" bool verifyRollbackLater() {
  return true;
}

namespace cartridge {
namespace {

// Image layout: image header, first segment header, esp_app_desc_t, then our info.
constexpr size_t kInfoOffset =
    sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);

const esp_partition_t *slot() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
}

bool readFrom(const esp_partition_t *part, CartridgeInfo &out) {
  esp_app_desc_t desc;
  if (!part || esp_ota_get_partition_description(part, &desc) != ESP_OK) return false;
  if (esp_partition_read(part, kInfoOffset, &out, sizeof(out)) != ESP_OK) return false;
  out.name[sizeof(out.name) - 1] = '\0';
  out.version[sizeof(out.version) - 1] = '\0';
  out.id[sizeof(out.id) - 1] = '\0';
  return out.magic == kCartridgeMagic;
}

// Boots `part` next (after confirming this firmware: a deliberate restart isn't a crash).
bool restartInto(const esp_partition_t *part, const char *what) {
  confirmHealthy();
  if (!part || esp_ota_set_boot_partition(part) != ESP_OK) {
    LOGE("cartridge", "can't boot %s", what);
    return false;
  }
  LOGI("cartridge", "restarting into %s", what);
  delay(100);
  esp_restart();
  return true;
}

}  // namespace

const esp_partition_t *launcherSlot() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
}

const esp_partition_t *rescueSlot() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
}

const CartridgeInfo &self() {
  return kDottyCartridge;
}

bool isLauncher() {
  return esp_ota_get_running_partition()->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1;
}

bool readInstalled(CartridgeInfo &out) {
  return readFrom(slot(), out);
}

bool readLauncher(CartridgeInfo &out) {
  return readFrom(launcherSlot(), out);
}

bool confirmHealthy() {
  esp_ota_img_states_t state;
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) return false;
  esp_ota_mark_app_valid_cancel_rollback();
  LOGI("cartridge", "%s %s confirmed (no rollback)", self().name, self().version);
  return true;
}

bool launcherBroken() {
  const esp_partition_t *part = launcherSlot();
  if (!part) return false;  // old layout: nothing to rescue with
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(part, &state) == ESP_OK &&
      (state == ESP_OTA_IMG_ABORTED || state == ESP_OTA_IMG_INVALID)) {
    return true;
  }
  CartridgeInfo info;
  return !readFrom(part, info);
}

bool eraseInstalled() {
  const esp_partition_t *part = slot();
  if (!part || esp_ota_set_boot_partition(launcherSlot()) != ESP_OK) return false;
  // Without its header the image is invalid: readInstalled() fails, the bootloader skips it.
  const bool ok = esp_partition_erase_range(part, 0, 4096) == ESP_OK;
  LOGI("cartridge", "installed cartridge erased: %s", ok ? "ok" : "FAILED");
  return ok;
}

void rebootToLauncher() {
  restartInto(launcherSlot(), "the launcher");
}

void rebootToRescue() {
  restartInto(rescueSlot(), "Rescue");
}

bool startInstalled() {
  return restartInto(slot(), "the installed cartridge");
}

}  // namespace cartridge
