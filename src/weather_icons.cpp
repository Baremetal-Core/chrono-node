#include "weather_icons.h"

#include <math.h>

#include "ui_theme.h"

namespace weather_icons {
namespace {

using theme::rgb;

struct Palette {
  uint16_t sun, rays, moon, cloud, darkCloud, rain, snow, bolt, fog;
};

constexpr Palette kColor = {
    rgb(255, 200, 0),    // sun
    rgb(255, 150, 0),    // rays
    rgb(255, 225, 140),  // moon
    rgb(215, 220, 232),  // cloud
    rgb(140, 146, 166),  // dark cloud
    rgb(0, 190, 255),    // rain
    rgb(235, 245, 255),  // snow
    rgb(255, 215, 0),    // bolt
    rgb(170, 175, 190),  // fog
};

void sun(TFT_eSprite& s, float cx, float cy, float r, const Palette& p) {
  for (int i = 0; i < 8; ++i) {
    float a = i * PI / 4;
    s.drawWideLine(cx + cosf(a) * r * 1.45f, cy + sinf(a) * r * 1.45f, cx + cosf(a) * r * 1.95f,
                   cy + sinf(a) * r * 1.95f, fmaxf(1.5f, r * 0.28f), p.rays);
  }
  s.fillSmoothCircle(cx, cy, r, p.sun);
}

void moon(TFT_eSprite& s, float cx, float cy, float r, const Palette& p, uint16_t bg) {
  s.fillSmoothCircle(cx, cy, r, p.moon);
  s.fillSmoothCircle(cx + r * 0.55f, cy - r * 0.4f, r * 0.85f, bg);
}

// Cloud of width w whose flat bottom is at y = by.
void cloud(TFT_eSprite& s, float cx, float by, float w, uint16_t c) {
  const float h = w * 0.26f;
  s.fillSmoothCircle(cx - w * 0.2f, by - h * 1.0f, w * 0.2f, c);
  s.fillSmoothCircle(cx + w * 0.1f, by - h * 1.35f, w * 0.27f, c);
  s.fillSmoothRoundRect(cx - w * 0.5f, by - h * 1.2f, w, h * 1.2f, h * 0.6f, c);
}

void drops(TFT_eSprite& s, float cx, float top, float size, int n, float len, uint16_t c) {
  const float step = size * 0.22f;
  for (int i = 0; i < n; ++i) {
    float x = cx + (i - (n - 1) / 2.0f) * step;
    s.drawWideLine(x + len * 0.25f, top, x - len * 0.25f, top + len, fmaxf(1.5f, size * 0.06f), c);
  }
}

void dots(TFT_eSprite& s, float cx, float top, float size, int n, uint16_t c) {
  const float step = size * 0.22f;
  for (int i = 0; i < n; ++i) {
    float x = cx + (i - (n - 1) / 2.0f) * step;
    float y = top + (i % 2) * size * 0.12f + size * 0.05f;
    s.fillSmoothCircle(x, y, fmaxf(1.2f, size * 0.05f), c);
  }
}

}  // namespace

void draw(TFT_eSprite& s, int16_t cx, int16_t cy, int16_t size, weather::Icon icon, bool isDay,
          bool mono, uint16_t monoColor) {
  Palette p = kColor;
  if (mono) p = {monoColor, monoColor, monoColor, monoColor, monoColor,
                 monoColor, monoColor, monoColor, monoColor};
  const uint16_t bg = s.readPixel(cx, cy - size / 2);
  const float w = size * 0.92f;
  const float cloudBottom = cy + size * 0.12f;  // cloud sits above any precipitation

  auto celestial = [&](float x, float y, float r) {
    if (isDay) sun(s, x, y, r, p);
    else moon(s, x, y, r, p, bg);
  };

  switch (icon) {
    case weather::Icon::Clear:
      celestial(cx, cy, size * 0.24f);
      break;
    case weather::Icon::PartlyCloudy:
      celestial(cx - size * 0.15f, cy - size * 0.13f, size * 0.18f);
      cloud(s, cx + size * 0.06f, cy + size * 0.3f, w * 0.78f, p.cloud);
      break;
    case weather::Icon::Cloudy:
      cloud(s, cx - size * 0.12f, cy + size * 0.05f, w * 0.6f, p.darkCloud);
      cloud(s, cx + size * 0.06f, cy + size * 0.3f, w * 0.8f, p.cloud);
      break;
    case weather::Icon::Fog:
      for (int i = 0; i < 4; ++i) {
        float y = cy - size * 0.27f + i * size * 0.18f;
        float inset = (i % 2) * size * 0.12f;
        s.drawWideLine(cx - size * 0.4f + inset, y, cx + size * 0.4f - inset * 0.5f, y,
                       fmaxf(2, size * 0.08f), p.fog);
      }
      break;
    case weather::Icon::Drizzle:
      cloud(s, cx, cloudBottom, w * 0.85f, p.cloud);
      dots(s, cx, cloudBottom + size * 0.08f, size, 4, p.rain);
      break;
    case weather::Icon::Rain:
      cloud(s, cx, cloudBottom, w * 0.85f, p.darkCloud);
      drops(s, cx, cloudBottom + size * 0.08f, size, 4, size * 0.24f, p.rain);
      break;
    case weather::Icon::Showers:
      celestial(cx + size * 0.2f, cy - size * 0.22f, size * 0.15f);
      cloud(s, cx - size * 0.04f, cloudBottom, w * 0.8f, p.cloud);
      drops(s, cx - size * 0.04f, cloudBottom + size * 0.08f, size, 3, size * 0.22f, p.rain);
      break;
    case weather::Icon::Thunder: {
      cloud(s, cx, cloudBottom, w * 0.85f, p.darkCloud);
      const float t = cloudBottom - size * 0.05f;
      const float u = size * 0.1f;
      s.fillTriangle(cx + u * 0.6f, t, cx - u * 1.2f, t + u * 2.2f, cx + u * 0.2f, t + u * 2.2f, p.bolt);
      s.fillTriangle(cx - u * 0.2f, t + u * 1.8f, cx + u * 1.0f, t + u * 1.8f, cx - u * 0.9f, t + u * 4.2f, p.bolt);
      break;
    }
    case weather::Icon::Snow:
      cloud(s, cx, cloudBottom, w * 0.85f, p.cloud);
      dots(s, cx, cloudBottom + size * 0.1f, size * 1.3f, 4, p.snow);
      break;
  }
}

}  // namespace weather_icons
