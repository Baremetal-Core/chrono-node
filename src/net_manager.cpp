#include "net_manager.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_mac.h>

#include <atomic>

#include "config.h"

namespace net {
namespace {

constexpr uint32_t kTaskStackBytes = 10 * 1024;
constexpr BaseType_t kTaskCore = 0;  // Arduino loop() runs on core 1
constexpr uint32_t kTaskPeriodMs = 10;

WiFiManager wm;
std::atomic<State> current{State::Connecting};
std::atomic<bool> portalRequested{false};
std::atomic<bool> forgetRequested{false};
char portalSsid[32] = "";

uint32_t attemptStartMs = 0;
uint32_t retryAtMs = 0;
uint32_t failures = 0;
bool portalOfferedThisBoot = false;
bool everConnected = false;
bool justProvisioned = false;  // credentials came from the portal and are untested
bool mdnsStarted = false;

// Set from the Wi-Fi event task, consumed in step().
volatile bool evtGotIp = false;
volatile bool evtDisconnected = false;
volatile uint8_t evtReason = 0;

void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      evtGotIp = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      evtReason = info.wifi_sta_disconnected.reason;
      evtDisconnected = true;
      break;
    default:
      break;
  }
}

void clearEvents() {
  evtGotIp = false;
  evtDisconnected = false;
}

void startConnect() {
  clearEvents();
  WiFi.mode(WIFI_STA);
  Serial.printf("[WIFI] Connecting to \"%s\"\n", wm.getWiFiSSID(true).c_str());
  WiFi.begin();  // uses credentials saved in NVS
  attemptStartMs = millis();
  current = State::Connecting;
}

void scheduleRetry() {
  uint32_t delayMs = config::kWifiRetryMinMs;
  for (uint32_t i = 1; i < failures && delayMs < config::kWifiRetryMaxMs; ++i) delayMs *= 2;
  if (delayMs > config::kWifiRetryMaxMs) delayMs = config::kWifiRetryMaxMs;
  retryAtMs = millis() + delayMs;
  current = State::WaitingRetry;
  Serial.printf("[WIFI] Retrying in %lus\n", (unsigned long)(delayMs / 1000));
}

void openPortal(uint32_t timeoutS) {
  clearEvents();
  WiFi.disconnect();
  wm.setConfigPortalTimeout(timeoutS);
  wm.startConfigPortal(portalSsid);  // non-blocking; serviced by wm.process()
  current = State::Portal;
  Serial.printf("[WIFI] Setup portal open: join Wi-Fi \"%s\", then browse to http://%s\n",
                portalSsid, WiFi.softAPIP().toString().c_str());
  if (timeoutS) Serial.printf("[WIFI] Portal closes in %lus if unused\n", (unsigned long)timeoutS);
}

void onConnected() {
  current = State::Connected;
  failures = 0;
  everConnected = true;
  justProvisioned = false;
  Serial.printf("[WIFI] Connected to \"%s\", IP %s, RSSI %d dBm\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  if (!mdnsStarted) {
    mdnsStarted = MDNS.begin(config::kHostname);
    if (mdnsStarted) Serial.printf("[WIFI] mDNS: %s.local\n", config::kHostname);
  }
}

void onAttemptFailed(const char* why) {
  ++failures;
  Serial.printf("[WIFI] Connection failed (%s)\n", why);
  WiFi.disconnect();
  clearEvents();
  if (justProvisioned) {
    // Credentials just entered in the portal don't work: let the user try again.
    justProvisioned = false;
    openPortal(0);
  } else if (!portalOfferedThisBoot && !everConnected) {
    // First failure since boot and never connected: offer setup in case the network changed.
    portalOfferedThisBoot = true;
    openPortal(config::kPortalTimeoutS);
  } else {
    scheduleRetry();
  }
}

void setup() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(portalSsid, sizeof(portalSsid), "%s%02X%02X", config::kPortalSsidPrefix, mac[4],
           mac[5]);

  WiFi.setHostname(config::kHostname);  // must precede WiFi.mode()
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // reconnects are handled here with backoff

  wm.setDebugOutput(false);
  wm.setConfigPortalBlocking(false);
  wm.setHostname(config::kHostname);
  wm.setTitle(config::kProjectName);
  wm.setDarkMode(true);
  wm.setSaveConnectTimeout(config::kPortalSaveConnectTimeoutS);
  // If the portal's own connection test fails, close it and let this module retry.
  wm.setBreakAfterConfig(true);
  // Called after any save from the portal, whether or not its connection test passed.
  wm.setSaveConfigCallback([] { justProvisioned = true; });
  wm.setWiFiAutoReconnect(false);
  // No firmware-update page: the portal AP is open.
  static const char* menu[] = {"wifi", "info", "exit"};
  wm.setMenu(menu, 3);

  if (wm.getWiFiIsSaved()) {
    startConnect();
  } else {
    Serial.println("[WIFI] No saved credentials");
    portalOfferedThisBoot = true;
    openPortal(0);
  }
}

void handleRequests() {
  if (forgetRequested.exchange(false)) {
    if (current == State::Portal) wm.stopConfigPortal();
    wm.resetSettings();
    Serial.println("[WIFI] Saved credentials erased");
    failures = 0;
    justProvisioned = false;
    openPortal(0);
  }
  if (portalRequested.exchange(false)) {
    if (current == State::Portal) {
      Serial.println("[WIFI] Setup portal already open");
    } else {
      openPortal(wm.getWiFiIsSaved() ? config::kPortalTimeoutS : 0);
    }
  }
}

void step() {
  handleRequests();
  switch (current.load()) {
    case State::Portal: {
      if (wm.process()) {
        clearEvents();
        onConnected();
      } else if (!wm.getConfigPortalActive()) {
        Serial.println("[WIFI] Setup portal closed");
        if (wm.getWiFiIsSaved()) {
          startConnect();
        } else {
          openPortal(0);
        }
      }
      break;
    }

    case State::Connecting:
      if (evtGotIp) {
        clearEvents();
        onConnected();
      } else if (evtDisconnected) {
        char why[24];
        snprintf(why, sizeof(why), "reason %u", evtReason);
        onAttemptFailed(why);
      } else if (millis() - attemptStartMs >= config::kWifiConnectTimeoutMs) {
        onAttemptFailed("timeout");
      }
      break;

    case State::Connected:
      if (evtDisconnected) {
        Serial.printf("[WIFI] Disconnected (reason %u)\n", evtReason);
        clearEvents();
        failures = 1;
        scheduleRetry();
      } else {
        evtGotIp = false;
      }
      break;

    case State::WaitingRetry:
      if ((int32_t)(millis() - retryAtMs) >= 0) startConnect();
      break;
  }
}

void taskMain(void*) {
  setup();
  for (;;) {
    step();
    vTaskDelay(pdMS_TO_TICKS(kTaskPeriodMs));
  }
}

}  // namespace

void begin() {
  xTaskCreatePinnedToCore(taskMain, "net", kTaskStackBytes, nullptr, 1, nullptr, kTaskCore);
}

State state() { return current; }

bool isConnected() { return current == State::Connected; }

void statusText(char* buf, size_t len) {
  switch (current.load()) {
    case State::Connected:
      snprintf(buf, len, "Wi-Fi: %s", WiFi.SSID().c_str());
      break;
    case State::Connecting:
      snprintf(buf, len, "Wi-Fi: connecting...");
      break;
    case State::WaitingRetry:
      snprintf(buf, len, "Wi-Fi: offline");
      break;
    case State::Portal:
      snprintf(buf, len, "Setup: join Wi-Fi %s", portalSsid);
      break;
  }
}

void printStatus() {
  static const char* names[] = {"connecting", "connected", "waiting to retry", "setup portal"};
  Serial.printf("[WIFI] State: %s, saved SSID: \"%s\", failures: %lu\n",
                names[static_cast<int>(current.load())], wm.getWiFiSSID(true).c_str(),
                (unsigned long)failures);
  if (current == State::Connected) {
    Serial.printf("[WIFI] IP %s, gateway %s, RSSI %d dBm, host %s.local\n",
                  WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
                  WiFi.RSSI(), config::kHostname);
  } else if (current == State::Portal) {
    Serial.printf("[WIFI] Portal SSID \"%s\", http://%s\n", portalSsid,
                  WiFi.softAPIP().toString().c_str());
  } else if (current == State::WaitingRetry) {
    Serial.printf("[WIFI] Next attempt in %lds\n", (long)(retryAtMs - millis()) / 1000);
  }
}

void requestPortal() { portalRequested = true; }

void requestForget() { forgetRequested = true; }

}  // namespace net
