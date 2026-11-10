// Chrono Node - PWM backlight with smooth, gamma-corrected fades.
#pragma once

#include <stdint.h>

namespace backlight {

// Takes over the backlight pin (TFT_BL) after the display is initialised.
void begin(uint8_t percent);

// Fades to `percent` (0-100) over `fadeMs`.
void fadeTo(uint8_t percent, uint32_t fadeMs);

// Advances a running fade; call every loop iteration.
void loop();

bool isFading();
uint8_t target();

}  // namespace backlight
