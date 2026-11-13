// Chrono Node - firmware updates over the network with rollback.
//
// Uploads go to the inactive OTA slot. Because the web UI has no password, an
// upload needs a one-time PIN that is shown on the clock's screen (proof of
// physical presence). A freshly installed image stays "pending" until it has
// run for kConfirmAfterMs; if it crashes or resets before that, the bootloader
// rolls back to the previous firmware.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ota {

// Main task.
void begin();
void loop();

// Network task: create a new PIN (valid 2 min). Returns it for logging.
uint32_t newPin();
// Any task: current PIN, or 0 when none is active.
uint32_t activePin();
// Consumes the PIN if it matches (single use).
bool checkPin(uint32_t pin);

// Install a GitHub release (network task). `tag` must look like "v1.2.3".
// Returns false if it's not a valid tag or an install is already running. The
// download happens in serviceNet(); the clock restarts when it succeeds.
bool requestRelease(const char* tag);
// Network task, while connected: runs a requested release install (blocks).
void serviceNet();
// Any task: why the last install failed, if it was in the last minute.
bool recentError(char* buf, size_t len);

// Upload progress, written by the network task: -1 idle, else 0-100.
void setProgress(int percent);
int progress();

void printStatus();

// How the flash chip is divided, for status displays (bytes).
struct FlashLayout {
  uint32_t chip = 0;        // whole chip
  uint32_t firmware = 0;    // running firmware image
  uint32_t slot = 0;        // the slot it runs from
  uint32_t updateSlot = 0;  // the other slot, kept free for updates
  uint32_t data = 0;        // LittleFS partition (photos)
  uint32_t system() const { return chip - slot - updateSlot - data; }  // boot, NVS, core dump
};
// Any task. The firmware size is measured once in begin().
FlashLayout flashLayout();

}  // namespace ota
