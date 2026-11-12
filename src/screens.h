// Chrono Node - screen navigation requests from outside the main loop.
#pragma once

// Switches to "clock", "forecast", "info" or "settings" as if swiped there.
// Returns false for an unknown name. Must be called from the main task.
bool requestScreenByName(const char* name);

// Scrolls the next banner message as soon as the clock face is showing.
void requestBannerNow();
