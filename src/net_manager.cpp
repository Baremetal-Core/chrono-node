#include "net_manager.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_mac.h>
#include <esp_sntp.h>
#include <esp_wifi.h>

#include <atomic>

#include "clock_sync.h"
#include "config.h"
#include "settings.h"
#include "weather.h"

namespace net {
namespace {

constexpr uint32_t kTaskStackBytes = 16 * 1024;  // TLS handshakes for weather
constexpr BaseType_t kTaskCore = 0;  // Arduino loop() runs on core 1
constexpr uint32_t kTaskPeriodMs = 10;

WiFiManager wm;
std::atomic<State> current{State::Connecting};
std::atomic<bool> portalRequested{false};
std::atomic<bool> forgetRequested{false};
std::atomic<bool> ntpRequested{false};
std::atomic<bool> scanRequested{false};
bool sntpStarted = false;
char ntpServer[config::kNtpServerMaxLen + 1] = "";  // SNTP keeps a pointer to this
char apSsid[32] = "";

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
uint8_t evtBssid[6] = {};
volatile uint8_t evtChannel = 0;

void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      evtGotIp = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      evtReason = info.wifi_sta_disconnected.reason;
      memcpy(evtBssid, info.wifi_sta_disconnected.bssid, 6);
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

// Office/mesh networks broadcast one SSID from many access points. Left to
// itself the ESP-IDF often joins a distant one (measured: -73 dBm while one
// at -49 dBm was in range), giving slow or failed connects. So scan first and
// pin the connection to the strongest AP by BSSID; after a failure the next
// strongest is tried. The pin is RAM-only; the saved credentials are unchanged.
constexpr int kMaxApCandidates = 3;
uint8_t pinnedBssid[6] = {};
bool pinned = false;
bool savedSsidVisible = true;  // saved network seen in the last pre-connect scan

void pinBestAp(uint32_t attempt) {
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return;
  conf.sta.bssid_set = 0;
  conf.sta.channel = 0;
  pinned = false;

  const String ssid(reinterpret_cast<const char*>(conf.sta.ssid));
  int best[kMaxApCandidates];
  int found = 0;
  int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < n; ++i) {
    if (WiFi.SSID(i) != ssid) continue;
    // Insertion into a short list sorted by RSSI, strongest first.
    int pos = found < kMaxApCandidates ? found++ : kMaxApCandidates;
    while (pos > 0 && WiFi.RSSI(best[pos - 1]) < WiFi.RSSI(i)) {
      if (pos < kMaxApCandidates) best[pos] = best[pos - 1];
      --pos;
    }
    if (pos < kMaxApCandidates) best[pos] = i;
  }
  if (found > 0) {
    // Each AP gets two attempts before moving on to the next strongest.
    const int pick = best[(attempt / 2) % found];
    memcpy(conf.sta.bssid, WiFi.BSSID(pick), 6);
    memcpy(pinnedBssid, conf.sta.bssid, 6);
    pinned = true;
    conf.sta.bssid_set = 1;
    conf.sta.channel = WiFi.channel(pick);
    Serial.printf("[WIFI] %d AP(s) for \"%s\"; using %s ch %ld (%ld dBm)\n", found, ssid.c_str(),
                  WiFi.BSSIDstr(pick).c_str(), (long)WiFi.channel(pick), (long)WiFi.RSSI(pick));
  } else {
    Serial.printf("[WIFI] \"%s\" not found in scan (%d networks)\n", ssid.c_str(), n);
  }
  // A failed scan (n < 0) says nothing about the network; don't treat it as absent.
  savedSsidVisible = found > 0 || n < 0;
  WiFi.scanDelete();
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_config(WIFI_IF_STA, &conf);
}

void startConnect() {
  clearEvents();
  WiFi.mode(WIFI_STA);
  pinBestAp(failures);
  Serial.printf("[WIFI] Connecting to \"%s\"\n", wm.getWiFiSSID(true).c_str());
  WiFi.begin();  // uses credentials saved in NVS
  attemptStartMs = millis();
  current = State::Connecting;
}

void scheduleRetry() {
  uint32_t delayMs = config::kWifiRetryMinMs;
  if (failures < config::kWifiQuickRetries) {
    delayMs = config::kWifiQuickRetryMs;
  } else {
    for (uint32_t i = config::kWifiQuickRetries; i < failures && delayMs < config::kWifiRetryMaxMs; ++i) {
      delayMs *= 2;
    }
  }
  if (delayMs > config::kWifiRetryMaxMs) delayMs = config::kWifiRetryMaxMs;
  retryAtMs = millis() + delayMs;
  current = State::WaitingRetry;
  Serial.printf("[WIFI] Retrying in %lus (attempt %lu)\n", (unsigned long)(delayMs / 1000),
                (unsigned long)failures + 1);
}

void openPortal(uint32_t timeoutS) {
  clearEvents();
  WiFi.disconnect();
  wm.setConfigPortalTimeout(timeoutS);
  wm.startConfigPortal(apSsid);  // non-blocking; serviced by wm.process()
  current = State::Portal;
  Serial.printf("[WIFI] Setup portal open: join Wi-Fi \"%s\", then browse to http://%s\n",
                apSsid, WiFi.softAPIP().toString().c_str());
  if (timeoutS) Serial.printf("[WIFI] Portal closes in %lus if unused\n", (unsigned long)timeoutS);
}

void onNtpSync(struct timeval*) { clock_sync::notifyNtpSync(); }

void startNtp() {
  strlcpy(ntpServer, settings::ntpServer(), sizeof(ntpServer));
  if (sntpStarted) sntp_stop();
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  sntp_setservername(0, ntpServer);
  sntp_set_sync_interval(config::kNtpSyncIntervalMs);
  sntp_set_time_sync_notification_cb(onNtpSync);
  sntp_init();
  sntpStarted = true;
  Serial.printf("[NTP] Syncing with %s\n", ntpServer);
}

void onConnected() {
  current = State::Connected;
  failures = 0;
  everConnected = true;
  justProvisioned = false;
  Serial.printf("[WIFI] Connected to \"%s\" (AP %s, ch %ld), IP %s, RSSI %d dBm\n",
                WiFi.SSID().c_str(), WiFi.BSSIDstr().c_str(), (long)WiFi.channel(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  if (!mdnsStarted) {
    mdnsStarted = MDNS.begin(config::kHostname);
    if (mdnsStarted) Serial.printf("[WIFI] mDNS: %s.local\n", config::kHostname);
  }
  startNtp();
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
  } else if (failures >= config::kWifiQuickRetries && !portalOfferedThisBoot && !everConnected &&
             !savedSsidVisible) {
    // Saved network not in range since boot: offer setup in case the network changed.
    // If it is visible, keep retrying: enterprise APs often reject a few attempts.
    portalOfferedThisBoot = true;
    openPortal(config::kPortalTimeoutS);
  } else {
    scheduleRetry();
  }
}

void setup() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(apSsid, sizeof(apSsid), "%s%02X%02X", config::kPortalSsidPrefix, mac[4],
           mac[5]);

  WiFi.setHostname(config::kHostname);  // must precede WiFi.mode()
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // reconnects are handled here with backoff
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

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

const char* authName(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise (unsupported)";
    case WIFI_AUTH_WPA3_PSK: return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3-PSK";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    default: return "other";
  }
}

void scanNetworks() {
  if (current == State::Connecting) {
    Serial.println("[WIFI] Busy connecting; try WIFI SCAN again in a few seconds");
    return;
  }
  Serial.println("[WIFI] Scanning (2.4 GHz only; 5 GHz networks are invisible to the ESP32)...");
  int n = WiFi.scanNetworks(false, true);
  if (n < 0) {
    Serial.printf("[WIFI] Scan failed (%d)\n", n);
    return;
  }
  for (int i = 0; i < n; ++i) {
    Serial.printf("[WIFI]   %-24s %s ch %2ld  %4ld dBm  %s\n",
                  WiFi.SSID(i).length() ? WiFi.SSID(i).c_str() : "(hidden)",
                  WiFi.BSSIDstr(i).c_str(), (long)WiFi.channel(i), (long)WiFi.RSSI(i),
                  authName(WiFi.encryptionType(i)));
  }
  Serial.printf("[WIFI] %d networks\n", n);
  WiFi.scanDelete();
}

void handleRequests() {
  if (scanRequested.exchange(false)) scanNetworks();
  if (forgetRequested.exchange(false)) {
    if (current == State::Portal) wm.stopConfigPortal();
    wm.resetSettings();
    Serial.println("[WIFI] Saved credentials erased");
    failures = 0;
    justProvisioned = false;
    openPortal(0);
  }
  if (ntpRequested.exchange(false)) {
    if (current == State::Connected) {
      startNtp();
    } else {
      Serial.println("[NTP] Wi-Fi not connected; will sync on connect");
    }
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
      } else if (evtDisconnected && pinned && memcmp((const void*)evtBssid, pinnedBssid, 6) != 0 &&
                 (evtBssid[0] | evtBssid[1] | evtBssid[2] | evtBssid[3] | evtBssid[4] | evtBssid[5])) {
        // Late event from the previous attempt's disconnect, not this one.
        evtDisconnected = false;
      } else if (evtDisconnected) {
        char why[48];
        snprintf(why, sizeof(why), "reason %u, AP %02X:%02X:%02X:%02X:%02X:%02X", evtReason,
                 evtBssid[0], evtBssid[1], evtBssid[2], evtBssid[3], evtBssid[4], evtBssid[5]);
        onAttemptFailed(why);
      } else if (millis() - attemptStartMs >= config::kWifiConnectTimeoutMs) {
        onAttemptFailed("timeout");
      }
      break;

    case State::Connected:
      if (!evtDisconnected) weather::service();
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

void ssid(char* buf, size_t len) {
  if (current == State::Connected) {
    strlcpy(buf, WiFi.SSID().c_str(), len);
  } else if (len) {
    buf[0] = '\0';
  }
}

int rssi() { return current == State::Connected ? WiFi.RSSI() : 0; }

const char* portalSsid() { return apSsid; }

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
    Serial.printf("[WIFI] Portal SSID \"%s\", http://%s\n", apSsid,
                  WiFi.softAPIP().toString().c_str());
  } else if (current == State::WaitingRetry) {
    Serial.printf("[WIFI] Next attempt in %lds\n", (long)(retryAtMs - millis()) / 1000);
  }
}

void requestPortal() { portalRequested = true; }

void requestForget() { forgetRequested = true; }

void requestNtpSync() { ntpRequested = true; }

void requestScan() { scanRequested = true; }

}  // namespace net
