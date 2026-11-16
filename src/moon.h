// Chrono Node - moon phase.
//
// The phase angle from Meeus' low-precision formulas (Astronomical
// Algorithms, ch. 48): the illuminated fraction is good to about 1%.
#pragma once

#include <stdint.h>
#include <time.h>

namespace moon {

struct Phase {
  float lit;      // illuminated fraction 0..1
  float angle;    // elongation 0..360°: 0 new, 90 first quarter, 180 full, 270 last quarter
  bool waxing;    // angle < 180
};

Phase at(time_t utc);

// "Waxing gibbous" etc., and the Vietnamese name ("Trăng khuyết đầu tháng").
const char* name(const Phase& p);
const char* nameVi(const Phase& p);

}  // namespace moon
