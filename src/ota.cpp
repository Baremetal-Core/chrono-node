#include "ota.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_random.h>

#include <atomic>

#include "certs/ca_bundle.h"
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
FlashLayout layout;

// Release install and update check: requested from any task, run on the
// network task. Guarded by requestLock.
portMUX_TYPE requestLock = portMUX_INITIALIZER_UNLOCKED;
bool releaseRequested = false;
char releaseTag[32] = "";
bool checkRequested = false;
CheckState checkResult = CheckState::Idle;
char checkText[64] = "";  // tag or error

void setCheck(CheckState state, const char* text) {
  portENTER_CRITICAL(&requestLock);
  checkResult = state;
  strlcpy(checkText, text, sizeof(checkText));
  portEXIT_CRITICAL(&requestLock);
}

// "v1.2.3" > "1.2.2"? Compares up to three numbers; missing ones are 0.
bool newerVersion(const char* tag, const char* current) {
  int a[3] = {}, b[3] = {};
  sscanf(tag[0] == 'v' ? tag + 1 : tag, "%d.%d.%d", &a[0], &a[1], &a[2]);
  sscanf(current, "%d.%d.%d", &b[0], &b[1], &b[2]);
  for (int i = 0; i < 3; ++i) {
    if (a[i] != b[i]) return a[i] > b[i];
  }
  return false;
}

// github.com/<repo>/releases/latest redirects to .../releases/tag/<tag>; the
// redirect names the newest release without using the (rate limited) API.
void runCheck() {
  setCheck(CheckState::Checking, "");
  char url[128];
  snprintf(url, sizeof(url), "https://github.com/%s/releases/latest", config::kReleaseRepo);
  WiFiClientSecure client;
  client.setCACertBundle(ca_bundle);
  client.setTimeout(15);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  http.useHTTP10(true);
  const char* headers[] = {"Location"};
  http.collectHeaders(headers, 1);
  if (!http.begin(client, url)) return setCheck(CheckState::Failed, "bad address");
  const int code = http.sendRequest("HEAD");
  const String location = http.header("Location");
  http.end();
  if (code < 0) return setCheck(CheckState::Failed, HTTPClient::errorToString(code).c_str());
  const int at = location.indexOf("/releases/tag/");
  if (code / 100 != 3 || at < 0) {
    return setCheck(CheckState::Failed, code / 100 == 3 ? "no releases yet" : "unexpected answer");
  }
  const String tag = location.substring(at + 14);
  Serial.printf("[OTA] Newest release: %s (running v%s)\n", tag.c_str(), config::kFirmwareVersion);
  setCheck(newerVersion(tag.c_str(), config::kFirmwareVersion) ? CheckState::Available
                                                               : CheckState::UpToDate,
           tag.c_str());
}
portMUX_TYPE errorLock = portMUX_INITIALIZER_UNLOCKED;
char lastError[80] = "";
uint32_t lastErrorMs = 0;

void fail(const char* fmt, ...) {
  char text[80];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  Serial.printf("[OTA] Install failed: %s\n", text);
  portENTER_CRITICAL(&errorLock);
  strlcpy(lastError, text, sizeof(lastError));
  lastErrorMs = millis() | 1;
  portEXIT_CRITICAL(&errorLock);
}

bool validTag(const char* tag) {
  if (strcmp(tag, "latest") == 0) return true;
  const size_t n = strlen(tag);
  if (n < 2 || n >= sizeof(releaseTag) || tag[0] != 'v') return false;
  for (size_t i = 1; i < n; ++i) {
    if (!isalnum((unsigned char)tag[i]) && !strchr(".-_", tag[i])) return false;
  }
  return true;
}

// Downloads the release asset straight into the update slot. GitHub answers
// with a redirect to its file host; both use TLS with certificate checks.
void installRelease() {
  char url[160];
  if (strcmp(releaseTag, "latest") == 0) {
    // GitHub redirects this to the newest release's file.
    snprintf(url, sizeof(url), "https://github.com/%s/releases/latest/download/%s",
             config::kReleaseRepo, config::kReleaseAsset);
  } else {
    snprintf(url, sizeof(url), "https://github.com/%s/releases/download/%s/%s",
             config::kReleaseRepo, releaseTag, config::kReleaseAsset);
  }
  Serial.printf("[OTA] Downloading %s\n", url);
  uploadProgress = 0;  // the clock shows "Updating firmware... 0%"

  WiFiClientSecure client;
  client.setCACertBundle(ca_bundle);
  client.setTimeout(20);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setRedirectLimit(5);
  http.setConnectTimeout(15000);
  http.setTimeout(20000);
  http.useHTTP10(true);
  http.setUserAgent(String(config::kFirmwareName) + "/" + config::kFirmwareVersion);
  if (!http.begin(client, url)) {
    uploadProgress = -1;
    return fail("bad download address");
  }
  const int code = http.GET();
  const int len = http.getSize();
  if (code != HTTP_CODE_OK) {
    http.end();
    uploadProgress = -1;
    if (code == HTTP_CODE_NOT_FOUND) return fail("%s has no %s", releaseTag, config::kReleaseAsset);
    return fail("download failed (%s)", code < 0 ? HTTPClient::errorToString(code).c_str()
                                                 : String(code).c_str());
  }
  if (len <= 0 || (uint32_t)len > layout.updateSlot) {
    http.end();
    uploadProgress = -1;
    return fail("unexpected file size (%d bytes)", len);
  }
  if (!Update.begin(len, U_FLASH)) {
    http.end();
    uploadProgress = -1;
    return fail("%s", Update.errorString());
  }
  WiFiClient* in = http.getStreamPtr();
  static uint8_t buf[2048];
  int done = 0;
  uint32_t lastDataMs = millis();
  while (done < len) {
    const int avail = in->available();
    if (avail > 0) {
      const int n = in->read(buf, min<int>(avail, sizeof(buf)));
      if (n <= 0) continue;
      if (Update.write(buf, n) != (size_t)n) break;
      done += n;
      lastDataMs = millis();
      uploadProgress = min(99, (int)((int64_t)done * 100 / len));
    } else if (!in->connected() || millis() - lastDataMs > 20000) {
      break;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  http.end();
  if (done != len) {
    const String why = Update.hasError() ? String(Update.errorString()) : String("connection lost");
    Update.abort();
    uploadProgress = -1;
    return fail("download stopped at %d%% (%s)", (int)((int64_t)done * 100 / len), why.c_str());
  }
  if (!Update.end(true)) {
    uploadProgress = -1;
    return fail("%s", Update.errorString());
  }
  uploadProgress = 100;
  Serial.printf("[OTA] Installed %s (%d bytes), restarting\n", releaseTag, len);
  Serial.flush();
  delay(500);
  ESP.restart();
}

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
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t* data =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  layout.chip = ESP.getFlashChipSize();
  layout.firmware = ESP.getSketchSize();  // reads the image headers: do it once
  layout.slot = running ? running->size : 0;
  layout.updateSlot = next ? next->size : 0;
  layout.data = data ? data->size : 0;
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

bool requestRelease(const char* tag) {
  if (uploadProgress >= 0 || !validTag(tag)) return false;
  portENTER_CRITICAL(&requestLock);
  const bool busy = releaseRequested;
  if (!busy) {
    strlcpy(releaseTag, tag, sizeof(releaseTag));
    releaseRequested = true;
  }
  portEXIT_CRITICAL(&requestLock);
  if (busy) return false;
  portENTER_CRITICAL(&errorLock);
  lastErrorMs = 0;  // a new attempt: forget the previous failure
  portEXIT_CRITICAL(&errorLock);
  return true;
}

void requestCheck() {
  portENTER_CRITICAL(&requestLock);
  checkRequested = true;
  checkResult = CheckState::Checking;
  portEXIT_CRITICAL(&requestLock);
}

CheckState checkState(char* tagOrError, size_t len) {
  portENTER_CRITICAL(&requestLock);
  const CheckState s = checkResult;
  if (tagOrError) strlcpy(tagOrError, checkText, len);
  portEXIT_CRITICAL(&requestLock);
  return s;
}

void serviceNet() {
  portENTER_CRITICAL(&requestLock);
  const bool install = releaseRequested, check = checkRequested;
  releaseRequested = checkRequested = false;
  portEXIT_CRITICAL(&requestLock);
  if (check) runCheck();
  if (install) installRelease();
}

bool recentError(char* buf, size_t len) {
  portENTER_CRITICAL(&errorLock);
  const bool recent = lastErrorMs && millis() - lastErrorMs < config::kOtaErrorShowMs;
  if (recent) strlcpy(buf, lastError, len);
  portEXIT_CRITICAL(&errorLock);
  return recent;
}

void setProgress(int percent) { uploadProgress = percent; }

int progress() { return uploadProgress; }

FlashLayout flashLayout() { return layout; }

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
