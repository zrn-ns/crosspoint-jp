#include "OtaRollback.h"

#include <Logging.h>
#include <esp_ota_ops.h>

// The Arduino core defines this weak, in C, in cores/esp32/esp32-hal-misc.c —
// and declares it in no header at all. `extern "C"` is what makes this an
// override rather than a separate C++-mangled function that nothing calls: get
// it wrong and the build still succeeds while initArduino() goes on cancelling
// the rollback at boot, exactly the bug this module exists to fix.
//
// scripts/check_rollback_hook.py fails the build if the linked symbol did not
// come from here, so the mistake cannot be made silently.
extern "C" bool verifyRollbackLater() { return true; }

namespace ota_rollback {
namespace {

bool readState(const esp_partition_t* partition, esp_ota_img_states_t* state) {
  return partition && esp_ota_get_state_partition(partition, state) == ESP_OK;
}

const char* stateName(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW:
      return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY:
      return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:
      return "VALID";
    case ESP_OTA_IMG_INVALID:
      return "INVALID";
    case ESP_OTA_IMG_ABORTED:
      return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:
      return "UNDEFINED";
    default:
      return "?";
  }
}

// Some other slot was given up on by the bootloader, i.e. this boot is a
// rollback. Scans the app partitions rather than assuming a two-slot layout.
bool otherSlotAborted(const esp_partition_t* running) {
  if (!running) return false;
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  bool aborted = false;
  while (it) {
    const esp_partition_t* part = esp_partition_get(it);
    esp_ota_img_states_t state;
    if (part != running && readState(part, &state) && state == ESP_OTA_IMG_ABORTED) {
      aborted = true;
      break;
    }
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  return aborted;
}

const char* bootState = "?";
bool rolledBack = false;

}  // namespace

void captureBootState() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  bootState = readState(running, &state) ? stateName(state) : "?";
  rolledBack = otherSlotAborted(running);
}

const char* bootStateName() { return bootState; }

void markCurrentAppValid() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (!readState(running, &state)) {
    LOG_ERR("BOOT", "Cannot read ota state; rollback confirmation skipped");
    return;
  }
  // Anything else is already confirmed (or is a factory image). Writing again
  // would erase and rewrite an otadata sector on every single boot.
  if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK) {
    LOG_ERR("BOOT", "Failed to confirm app: %s", esp_err_to_name(err));
    return;
  }
  LOG_INF("BOOT", "App confirmed, rollback cancelled (%s)", running->label);
}

bool didRollBack() { return rolledBack; }

const char* runningPartitionLabel() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  return running ? running->label : "?";
}

}  // namespace ota_rollback
