// Chrono Node - Wi-Fi connection management and provisioning.
//
// Runs in its own FreeRTOS task so connecting, scanning and the captive
// portal never pause the clock. Connects with credentials saved in NVS and
// retries with exponential backoff. A WiFiManager captive portal is opened
// when no credentials exist, when the saved network is not in range after
// boot, when newly entered credentials fail, or on request. Networks that are
// in range but reject attempts are retried with backoff. SNTP is started on
// every (re)connect and reports to clock_sync.
#pragma once

#include <stddef.h>

namespace net {

enum class State { Connecting, Connected, WaitingRetry, Portal };

// Starts the network task. Call once from setup().
void begin();

State state();
bool isConnected();

// Connected network name (empty when not connected) and signal strength.
void ssid(char* buf, size_t len);
int rssi();
void ip(char* buf, size_t len);  // empty when not connected

// Name of the setup portal network, e.g. "Chrono-Node-57A1".
const char* portalSsid();

void printStatus();
// Requests are handled asynchronously by the network task.
void requestPortal();     // open the setup portal (auto-closes if credentials exist)
void requestForget();     // erase saved credentials and open the portal
void requestNtpSync();    // (re)start SNTP with the configured server; syncs now
void requestScan();       // log visible 2.4 GHz networks with BSSID, channel, RSSI, security

}  // namespace net
