#include "cartridge.h"

#include <esp_app_desc.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "log.h"

extern "C" const CartridgeInfo kDottyCartridge;  // defined by DOTTY_CARTRIDGE()

namespace cartridge {
namespace {

// Image layout: image header, first segment header, esp_app_desc_t, then our info.
constexpr size_t kInfoOffset =
    sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);

const esp_partition_t *slot() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
}

}  // namespace

const CartridgeInfo &self() {
  return kDottyCartridge;
}

bool isLauncher() {
  return esp_ota_get_running_partition()->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY;
}

bool readInstalled(CartridgeInfo &out) {
  const esp_partition_t *part = slot();
  esp_app_desc_t desc;
  if (!part || esp_ota_get_partition_description(part, &desc) != ESP_OK) return false;
  if (esp_partition_read(part, kInfoOffset, &out, sizeof(out)) != ESP_OK) return false;
  out.name[sizeof(out.name) - 1] = '\0';
  out.version[sizeof(out.version) - 1] = '\0';
  out.id[sizeof(out.id) - 1] = '\0';
  return out.magic == kCartridgeMagic;
}

void rebootToLauncher() {
  const esp_partition_t *factory =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!factory || esp_ota_set_boot_partition(factory) != ESP_OK) {
    LOGE("cartridge", "no launcher partition");
    return;
  }
  LOGI("cartridge", "rebooting into the launcher");
  delay(100);
  esp_restart();
}

bool startInstalled() {
  const esp_partition_t *part = slot();
  if (!part || esp_ota_set_boot_partition(part) != ESP_OK) {
    LOGE("cartridge", "no valid cartridge to start");
    return false;
  }
  LOGI("cartridge", "starting the installed cartridge");
  delay(100);
  esp_restart();
  return true;
}

}  // namespace cartridge
