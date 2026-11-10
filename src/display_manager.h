// Chrono Node - TFT rendering for the CYD display.
#pragma once

#include <stdint.h>
#include <time.h>

class DisplayManager {
 public:
  bool begin();

  // Draws local HH:MM:SS (24h) and DD/MM/YYYY. Only changed regions are redrawn.
  void showTime(const struct tm& local);

  // Full-screen error message. Redrawn only when the message changes.
  void showError(const char* title, const char* detail);

  // Small status line at the bottom of the clock screen; pass nullptr to clear.
  void setStatus(const char* text);

 private:
  enum class Mode { None, Clock, Error };
  void enterMode(Mode mode);

  Mode mode_ = Mode::None;
  bool spriteOk_ = false;
  char lastTime_[9] = "";
  char lastDate_[11] = "";
  char lastStatus_[64] = "";
  char lastError_[96] = "";
};
