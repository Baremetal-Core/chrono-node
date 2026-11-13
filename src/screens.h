// Chrono Node - screen navigation requests from outside the main loop.
#pragma once

// Switches to "clock", "forecast", "photo", "info" or "settings" as if swiped there.
// Returns false for an unknown name. Must be called from the main task.
bool requestScreenByName(const char* name);

// Shows photo `slot` (0 = the next one in rotation) or the forecast for their
// set time, as if the slideshow/forecast cycle had brought it up.
void requestPhotoNow(int slot);
void requestForecastNow();

// Shows the forecast and then each photo once, even at night. Returns the
// number of screens queued (0: no weather data and no photos).
int startPreview();

// Scrolls the next banner message as soon as the clock face is showing.
void requestBannerNow();
