#include "moon.h"

#include <math.h>

namespace moon {
namespace {

constexpr double kDr = M_PI / 180;

double sinDeg(double deg) { return sin(deg * kDr); }

int octant(const Phase& p) { return static_cast<int>(fmod(p.angle + 22.5, 360.0) / 45.0); }

}  // namespace

Phase at(time_t utc) {
  const double t = (utc / 86400.0 + 2440587.5 - 2451545.0) / 36525;  // centuries since J2000
  const double t2 = t * t, t3 = t2 * t, t4 = t3 * t;
  const double d = 297.8501921 + 445267.1114034 * t - 0.0018819 * t2 + t3 / 545868 - t4 / 113065000;
  const double m = 357.5291092 + 35999.0502909 * t - 0.0001536 * t2 + t3 / 24490000;
  const double mp = 134.9633964 + 477198.8675055 * t + 0.0087414 * t2 + t3 / 69699 - t4 / 14712000;
  // Phase angle (0° at full moon), from the mean elongation plus the main terms.
  const double i = 180 - d - 6.289 * sinDeg(mp) + 2.100 * sinDeg(m) - 1.274 * sinDeg(2 * d - mp) -
                   0.658 * sinDeg(2 * d) - 0.214 * sinDeg(2 * mp) - 0.110 * sinDeg(d);
  Phase p;
  p.lit = static_cast<float>((1 + cos(i * kDr)) / 2);
  p.angle = static_cast<float>(fmod(fmod(180 - i, 360.0) + 360.0, 360.0));  // the true elongation
  p.waxing = p.angle < 180;
  return p;
}

const char* name(const Phase& p) {
  static const char* const kNames[] = {"New moon",  "Waxing crescent", "First quarter",
                                       "Waxing gibbous", "Full moon", "Waning gibbous",
                                       "Last quarter", "Waning crescent"};
  return kNames[octant(p) % 8];
}

const char* nameVi(const Phase& p) {
  static const char* const kNames[] = {"Trăng non",
                                       "Trăng lưỡi liềm đầu tháng",
                                       "Trăng bán nguyệt đầu tháng",
                                       "Trăng khuyết đầu tháng",
                                       "Trăng tròn",
                                       "Trăng khuyết cuối tháng",
                                       "Trăng bán nguyệt cuối tháng",
                                       "Trăng lưỡi liềm cuối tháng"};
  return kNames[octant(p) % 8];
}

}  // namespace moon
