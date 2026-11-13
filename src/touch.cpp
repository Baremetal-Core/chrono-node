#include "touch.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SPI.h>
#include <math.h>

#include "config.h"

namespace touch {
namespace {

SPIClass bus(VSPI);
const SPISettings kSpi(2000000, MSBFIRST, SPI_MODE0);

// Generic fallback for a CYD in landscape until TOUCH CAL has been run.
Calibration cal = {320.0f / 2900, 0, -55, 0, 240.0f / 2470, -96};
constexpr const char* kCalKey = "touch_aff";

constexpr uint32_t kPollMs = 15;
constexpr uint8_t kReleaseSamples = 3;   // consecutive "not pressed" polls to end a touch
constexpr int16_t kSwipeMinPx = 60;
constexpr int16_t kTapMaxMovePx = 30;
constexpr uint32_t kTapMaxMs = 800;

uint32_t lastPollMs = 0;
bool down = false;
uint8_t upCount = 0;
uint32_t downMs = 0;
Point start = {0, 0};
Point last = {0, 0};
bool havePos = false;
bool held = false;  // this touch already reported Hold

int16_t bestTwoAvg(int16_t a, int16_t b, int16_t c) {
  int16_t ab = abs(a - b), ac = abs(a - c), bc = abs(b - c);
  if (ab <= ac && ab <= bc) return (a + b) / 2;
  if (ac <= ab && ac <= bc) return (a + c) / 2;
  return (b + c) / 2;
}

// Solves a 3x3 linear system with Cramer's rule.
bool solve3(const double m[3][3], const double v[3], double out[3]) {
  auto det = [](const double a[3][3]) {
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
           a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  };
  const double d = det(m);
  if (fabs(d) < 1e-9) return false;
  for (int c = 0; c < 3; ++c) {
    double t[3][3];
    for (int r = 0; r < 3; ++r)
      for (int k = 0; k < 3; ++k) t[r][k] = k == c ? v[r] : m[r][k];
    out[c] = det(t) / d;
  }
  return true;
}

void loadCalibration() {
  Preferences p;
  if (!p.begin("chrono-node", true)) return;
  if (p.isKey(kCalKey) && p.getBytesLength(kCalKey) == sizeof(Calibration)) {
    p.getBytes(kCalKey, &cal, sizeof(cal));
  }
  p.end();
}

}  // namespace

void begin() {
  pinMode(config::kTouchCsPin, OUTPUT);
  digitalWrite(config::kTouchCsPin, HIGH);
  bus.begin(config::kTouchClkPin, config::kTouchMisoPin, config::kTouchMosiPin, config::kTouchCsPin);
  loadCalibration();
}

bool readRaw(uint16_t& rx, uint16_t& ry, uint16_t& z) {
  int16_t d[6];
  bus.beginTransaction(kSpi);
  digitalWrite(config::kTouchCsPin, LOW);
  bus.transfer(0xB1);  // Z1
  int32_t z1 = bus.transfer16(0xC1) >> 3;
  int32_t z2 = bus.transfer16(0x91) >> 3;  // Z2, then start X
  int32_t pressure = z1 + 4095 - z2;
  if (pressure >= config::kTouchPressure) {
    bus.transfer16(0x91);  // discard the first X reading (settling)
    d[0] = bus.transfer16(0xD1) >> 3;
    d[1] = bus.transfer16(0x91) >> 3;
    d[2] = bus.transfer16(0xD1) >> 3;
    d[3] = bus.transfer16(0x91) >> 3;
  }
  d[4] = bus.transfer16(0xD0) >> 3;  // last Y; power down with the pen IRQ enabled
  d[5] = bus.transfer16(0) >> 3;
  digitalWrite(config::kTouchCsPin, HIGH);
  bus.endTransaction();
  if (pressure < config::kTouchPressure) return false;
  rx = bestTwoAvg(d[0], d[2], d[4]);
  ry = bestTwoAvg(d[1], d[3], d[5]);
  z = static_cast<uint16_t>(constrain(pressure, 0, 65535));
  return true;
}

Point map(uint16_t rx, uint16_t ry) {
  float x = cal.ax * rx + cal.bx * ry + cal.cx;
  float y = cal.ay * rx + cal.by * ry + cal.cy;
  return {static_cast<int16_t>(constrain(lroundf(x), 0, 319)),
          static_cast<int16_t>(constrain(lroundf(y), 0, 239))};
}

bool fitCalibration(const Point* screen, const uint16_t* rx, const uint16_t* ry, int n,
                    Calibration& out) {
  if (n < 3) return false;
  // Normal equations for [a b c] in a*rx + b*ry + c = target.
  double m[3][3] = {}, vx[3] = {}, vy[3] = {};
  for (int i = 0; i < n; ++i) {
    const double r[3] = {double(rx[i]), double(ry[i]), 1.0};
    for (int a = 0; a < 3; ++a) {
      for (int b = 0; b < 3; ++b) m[a][b] += r[a] * r[b];
      vx[a] += r[a] * screen[i].x;
      vy[a] += r[a] * screen[i].y;
    }
  }
  double sx[3], sy[3];
  if (!solve3(m, vx, sx) || !solve3(m, vy, sy)) return false;
  out = {float(sx[0]), float(sx[1]), float(sx[2]), float(sy[0]), float(sy[1]), float(sy[2])};
  return true;
}

const Calibration& calibration() { return cal; }

void setCalibration(const Calibration& c) {
  cal = c;
  Preferences p;
  if (p.begin("chrono-node", false)) {
    p.putBytes(kCalKey, &cal, sizeof(cal));
    p.end();
  }
}

Event poll() {
  Event ev;
  const uint32_t now = millis();
  if (now - lastPollMs < kPollMs) return ev;
  lastPollMs = now;

  uint16_t rx, ry, z;
  if (readRaw(rx, ry, z)) {
    upCount = 0;
    if (!down) {
      down = true;
      downMs = now;
      havePos = false;
      held = false;
      ev.gesture = Gesture::Down;
    }
    if (z >= config::kTouchFirmPressure) {
      Point p = map(rx, ry);
      if (!havePos) {
        havePos = true;
        start = p;
      }
      last = p;
    }
    if (ev.gesture == Gesture::Down) {
      ev.at = havePos ? start : Point{0, 0};
    } else if (!held && havePos && now - downMs >= config::kTouchHoldMs &&
               abs(last.x - start.x) <= kTapMaxMovePx && abs(last.y - start.y) <= kTapMaxMovePx) {
      held = true;
      ev.gesture = Gesture::Hold;
      ev.at = start;
    }
    return ev;
  }
  if (!down || ++upCount < kReleaseSamples) return ev;

  down = false;
  if (!havePos || held) return ev;  // no reliable position, or already reported as Hold
  const int16_t dx = last.x - start.x;
  const int16_t dy = last.y - start.y;
  ev.at = start;
  if (abs(dx) >= kSwipeMinPx && abs(dx) > 2 * abs(dy)) {
    ev.gesture = dx < 0 ? Gesture::SwipeLeft : Gesture::SwipeRight;
  } else if (abs(dx) <= kTapMaxMovePx && abs(dy) <= kTapMaxMovePx && now - downMs <= kTapMaxMs) {
    ev.gesture = Gesture::Tap;
  }
  return ev;
}

}  // namespace touch
