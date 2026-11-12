// Chrono Node - XPT2046 resistive touch on the CYD's dedicated SPI bus.
//
// Readings are filtered (best two of three), mapped to screen pixels with a
// calibration saved in NVS, and turned into gestures: tap and swipe. Only
// firm presses give positions: light pressure (finger landing or lifting)
// reads unreliable coordinates on a resistive panel.
#pragma once

#include <stdint.h>

namespace touch {

struct Point {
  int16_t x;
  int16_t y;
};

// Affine map from controller to screen coordinates:
//   x = ax * rx + bx * ry + cx,  y = ay * rx + by * ry + cy
// This also corrects a touch panel that is slightly rotated or skewed
// relative to the display (measured on this CYD).
struct Calibration {
  float ax, bx, cx;
  float ay, by, cy;
};

// Least-squares fit of a calibration from `n` (>= 3) screen/raw point pairs.
bool fitCalibration(const Point* screen, const uint16_t* rx, const uint16_t* ry, int n,
                    Calibration& out);

enum class Gesture : uint8_t { None, Down, Tap, SwipeLeft, SwipeRight };

struct Event {
  Gesture gesture = Gesture::None;
  Point at = {0, 0};  // tap position, or where the touch started
};

void begin();

// Polls the controller (rate-limited internally); call every loop iteration.
// `Down` is reported once when a touch starts, the gesture on release.
Event poll();

// One filtered raw reading. Returns false if the screen isn't pressed.
bool readRaw(uint16_t& rx, uint16_t& ry, uint16_t& z);

Point map(uint16_t rx, uint16_t ry);

const Calibration& calibration();
void setCalibration(const Calibration& cal);  // also saves to NVS

}  // namespace touch
