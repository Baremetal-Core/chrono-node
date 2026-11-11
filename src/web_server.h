// Chrono Node - LAN web UI and JSON API at http://chrono-node.local
//
// Runs in the network task while Wi-Fi is connected (it is stopped while the
// setup portal owns port 80). Settings changes are sent as console commands
// to the main task, so they go through the same validation as the serial
// console and all I2C/display work stays in one task.
//
//   GET  /                 settings page
//   GET  /api/status       live status (JSON)
//   GET  /api/settings     current settings (JSON)
//   GET  /api/geocode?q=   top 5 location matches from Open-Meteo (JSON)
//   POST /api/command      body: one whitelisted console command (text/plain)
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace web {

// Network task.
void start();
void stop();
void loop();

// Main task: next queued console command, if any.
bool nextCommand(char* buf, size_t len);

// Main task: state only the main task knows, published once per second.
struct ClockState {
  bool rtcPresent;
  bool rtcLostPower;
  bool hasTemp;
  float tempC;
  bool night;
  char screen[12];
};
void publishClockState(const ClockState& state);

}  // namespace web
