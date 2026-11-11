#include "ota.h"

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_random.h>

#include <atomic>

#include "config.h"

// Tell the Arduino core not to confirm a new image at startup; ota::loop()
// does it once the firmware has proven to run.
extern "C" bool verifyRollbackLater() { return true; }

namespace ota {
namespace {

std::atomic<uint32_t> pin{0};
std::atomic<uint32_t> pinExpiresMs{0};
std::atomic<int> uploadProgress{-1};
bool pendingVerify = false;

const char* stateName(esp_ota_img_states_t s) {
  switch (s) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
  }
}

}  // namespace

void begin() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    pendingVerify = true;
    Serial.printf("[OTA] New firmware on %s: confirming after %lus of normal running\n",
                  running->label, (unsigned long)(config::kOtaConfirmAfterMs / 1000));
  }
}

void loop() {
  if (pendingVerify && millis() >= config::kOtaConfirmAfterMs) {
    pendingVerify = false;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      Serial.println("[OTA] New firmware confirmed");
    } else {
      Serial.println("[OTA] ERROR: could not confirm firmware");
    }
  }
  if (pin && (int32_t)(millis() - pinExpiresMs) >= 0) pin = 0;
}

uint32_t newPin() {
  uint32_t p = 1000 + esp_random() % 9000;
  pinExpiresMs = millis() + config::kOtaPinValidMs;
  pin = p;
  return p;
}

uint32_t activePin() { return pin; }

bool checkPin(uint32_t candidate) {
  uint32_t p = pin;
  if (p == 0 || candidate != p) return false;
  pin = 0;
  return true;
}

void setProgress(int percent) { uploadProgress = percent; }

int progress() { return uploadProgress; }

void printStatus() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running) esp_ota_get_state_partition(running, &state);
  Serial.printf("[OTA] Running %s (%s), next update goes to %s (%lu KB)\n",
                running ? running->label : "?", stateName(state), next ? next->label : "?",
                next ? (unsigned long)(next->size / 1024) : 0UL);
  Serial.printf("[OTA] Firmware %s v%s (%s), size %u KB\n", config::kFirmwareName,
                config::kFirmwareVersion, config::kGitRevision, ESP.getSketchSize() / 1024);
}

}  // namespace ota
