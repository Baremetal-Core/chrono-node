// Chrono Node - Wi-Fi connection management and provisioning.
//
// Runs in its own FreeRTOS task so connecting, scanning and the captive
// portal never pause the clock. Connects with credentials saved in NVS and
// retries with exponential backoff. A WiFiManager captive portal is opened
// when no credentials exist, when the first connection after boot fails,
// when newly entered credentials fail, or on request.
#pragma once

#include <stddef.h>

namespace net {

enum class State { Connecting, Connected, WaitingRetry, Portal };

// Starts the network task. Call once from setup().
void begin();

State state();
bool isConnected();

// Short human-readable status for the display, e.g. "Wi-Fi: MyNetwork".
void statusText(char* buf, size_t len);

void printStatus();
// Requests are handled asynchronously by the network task.
void requestPortal();     // open the setup portal (auto-closes if credentials exist)
void requestForget();     // erase saved credentials and open the portal

}  // namespace net
