#include "backlight.h"

#include <Arduino.h>
#include <math.h>

#include "config.h"

namespace backlight {
namespace {

constexpr uint32_t kMaxDuty = (1u << config::kBacklightPwmBits) - 1;

float current = 0;  // perceived level 0..100
float from = 0;
uint8_t to = 0;
uint32_t startMs = 0;
uint32_t durationMs = 0;
bool fading = false;

// Perceived brightness is roughly a power curve of PWM duty, so low levels get
// fine steps and 50% looks like half brightness.
void apply(float percent) {
  float duty = powf(percent / 100.0f, 2.2f) * kMaxDuty;
  uint32_t d = static_cast<uint32_t>(duty + 0.5f);
  if (percent > 0 && d == 0) d = 1;
  ledcWrite(config::kBacklightPwmChannel, d);
}

}  // namespace

void begin(uint8_t percent) {
  ledcSetup(config::kBacklightPwmChannel, config::kBacklightPwmHz, config::kBacklightPwmBits);
  ledcAttachPin(config::kBacklightPin, config::kBacklightPwmChannel);
  current = to = percent > 100 ? 100 : percent;
  apply(current);
}

void fadeTo(uint8_t percent, uint32_t fadeMs) {
  if (percent > 100) percent = 100;
  from = current;
  to = percent;
  startMs = millis();
  durationMs = fadeMs;
  fading = true;
  if (fadeMs == 0) loop();
}

void loop() {
  if (!fading) return;
  uint32_t elapsed = millis() - startMs;
  if (elapsed >= durationMs) {
    current = to;
    fading = false;
  } else {
    current = from + (to - from) * (static_cast<float>(elapsed) / durationMs);
  }
  apply(current);
}

bool isFading() { return fading; }

uint8_t target() { return to; }

}  // namespace backlight
