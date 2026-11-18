#include "backdrop.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <TJpg_Decoder.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace backdrop {
namespace {

constexpr int W = 320, H = 240;

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
inline uint16_t swap16(uint16_t c) { return (c >> 8) | (c << 8); }

// a..b by t/256 (t 0..256): cheaper than theme::mix (no division).
inline uint16_t blend(uint16_t a, uint16_t b, int t) {
  if (t <= 0) return a;
  if (t >= 256) return b;
  const int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  const int r = ar + ((((b >> 11) - ar) * t) >> 8);
  const int g = ag + (((((b >> 5) & 63) - ag) * t) >> 8);
  const int bl = ab + ((((b & 31) - ab) * t) >> 8);
  return (r << 11) | (g << 5) | bl;
}

uint32_t rng = 1;
uint32_t rnd() {
  rng = rng * 1664525u + 1013904223u;
  return rng >> 8;
}
int rndIn(int lo, int hi) { return lo + static_cast<int>(rnd() % static_cast<uint32_t>(hi - lo + 1)); }
float rndF() { return (rnd() & 0xFFFF) / 65535.0f; }

Theme theme = Theme::None;
bool night = false;
int dim = 256;  // scales every light: 256 by day, less at night
uint16_t base = 0;
uint16_t acc1 = 0, acc2 = 0;
// The vertical gradient every theme starts from, kept in 8-bit channels
// (pre-scaled to 16.16 of the 5/6-bit range) and ordered-dithered per pixel:
// dark gradients would show bands in 16-bit colour otherwise.
uint32_t gradR[H], gradG[H], gradB[H];
uint8_t* mem = nullptr;
constexpr uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

inline uint16_t baseAt(int x, int y) {
  const uint32_t d = (kBayer[y & 3][x & 3] * 2 + 1) * 2048;
  return ((gradR[y] + d) >> 16) << 11 | ((gradG[y] + d) >> 16) << 5 | ((gradB[y] + d) >> 16);
}

void gradient(uint16_t top, uint16_t mid, uint16_t bottom, int midY) {
  auto ch = [](uint16_t c, int i) {  // 565 -> 8-bit channel
    return i == 0 ? (c >> 11) * 255 / 31 : i == 1 ? ((c >> 5) & 63) * 255 / 63 : (c & 31) * 255 / 31;
  };
  for (int y = 0; y < H; ++y) {
    const uint16_t a = y < midY ? top : mid, b = y < midY ? mid : bottom;
    const int t = y < midY ? y * 256 / midY : (y - midY) * 256 / (H - midY);
    int v[3];
    for (int i = 0; i < 3; ++i) v[i] = ch(a, i) + (ch(b, i) - ch(a, i)) * t / 256;
    gradR[y] = v[0] * 31u * 257u;
    gradG[y] = v[1] * 63u * 257u;
    gradB[y] = v[2] * 31u * 257u;
  }
}

// ---- Stars (Night sky and Synthwave's sky) ----

struct Star {
  int16_t x;
  uint8_t y, bright, tint, cur;  // cur: brightness now (twinklers change it)
  uint16_t periodMs, phase;
  bool twinkles, big;
};
constexpr uint16_t kStarTints[] = {rgb(255, 255, 255), rgb(190, 215, 255), rgb(255, 228, 190)};

struct Stars {
  int count;
  Star star[130];
  uint8_t rowFirst[H + 1];  // stars are sorted by y: row y's are rowFirst[y]..rowFirst[y+1]-1
};

void makeStars(Stars& s, int count, int maxY, int twinklers) {
  s.count = count;
  for (int i = 0; i < count; ++i) {
    Star& st = s.star[i];
    st.x = rndIn(0, W - 1);
    st.y = rndIn(0, maxY);
    const float r = rndF();
    st.bright = static_cast<uint8_t>(70 + r * r * r * 185);
    st.tint = rnd() % 3;
    st.big = st.bright > 215;
    st.twinkles = false;
    st.cur = st.bright;
  }
  for (int i = 0; i < twinklers; ++i) {  // among the brighter, not the big ones
    Star& st = s.star[rnd() % count];
    if (st.big) continue;
    st.twinkles = true;
    st.bright = max<uint8_t>(st.bright, 150);
    st.periodMs = rndIn(1400, 4200);
    st.phase = rnd() % st.periodMs;
  }
  qsort(s.star, count, sizeof(Star), [](const void* a, const void* b) {
    return static_cast<const Star*>(a)->y - static_cast<const Star*>(b)->y;
  });
  int i = 0;
  for (int y = 0; y <= H; ++y) {
    while (i < count && s.star[i].y < y) ++i;
    s.rowFirst[y] = i;
  }
}

// Stars over a row already filled (swapped) in out[0..w).
void paintStars(const Stars& s, int16_t y, int16_t x0, int16_t w, uint16_t* out) {
  for (int dy = -1; dy <= 1; ++dy) {
    const int sy = y - dy;
    if (sy < 0 || sy >= H) continue;
    for (int i = s.rowFirst[sy]; i < s.rowFirst[sy + 1]; ++i) {
      const Star& st = s.star[i];
      const int a = st.cur * dim >> 8;
      auto put = [&](int x, int alpha) {
        if (x < x0 || x >= x0 + w) return;
        out[x - x0] = swap16(blend(swap16(out[x - x0]), kStarTints[st.tint], alpha));
      };
      if (dy == 0) {
        put(st.x, a);
        if (st.big) {
          put(st.x - 1, a / 3);
          put(st.x + 1, a / 3);
        }
      } else if (st.big) {
        put(st.x, a / 3);
      }
    }
  }
}

// ---- Glow: soft light of the style's colours behind the time ----

struct Glow {
  uint8_t gx1[W], gy1[H], gx2[W], gy2[H];
};

void gauss(uint8_t* t, int n, float centre, float sigma) {
  for (int i = 0; i < n; ++i) {
    const float d = (i - centre) / sigma;
    t[i] = static_cast<uint8_t>(255 * expf(-d * d));
  }
}

void setupGlow() {
  Glow& g = *reinterpret_cast<Glow*>(mem);
  gauss(g.gx1, W, 160, 150);
  gauss(g.gy1, H, 96, 92);
  gauss(g.gx2, W, 300 - rndIn(0, 60), 120);
  gauss(g.gy2, H, 236, 70);
  gradient(base, base, blend(base, rgb(0, 0, 0), 90), 120);
}

void rowGlow(int16_t y, int16_t x0, int16_t w, uint16_t* out) {
  const Glow& g = *reinterpret_cast<const Glow*>(mem);
  const int s1 = 100 * dim >> 8, s2 = 70 * dim >> 8;
  for (int i = 0; i < w; ++i) {
    const int x = x0 + i;
    uint16_t c = blend(baseAt(x, y), acc1, (g.gx1[x] * g.gy1[y] * s1) >> 16);
    c = blend(c, acc2, (g.gx2[x] * g.gy2[y] * s2) >> 16);
    out[i] = swap16(c);
  }
}

// ---- Night sky: stars over a deep gradient, a faint milky way ----

constexpr int kNoiseW = W / 8 + 2, kNoiseH = H / 8 + 2;
struct Sky {
  Stars stars;
  uint8_t noise[kNoiseH][kNoiseW];
  float slope, offset;
};

uint8_t noiseAt(const uint8_t (*n)[kNoiseW], int x, int y) {  // bilinear value noise
  const int ix = x >> 3, iy = y >> 3, tx = (x & 7) * 32, ty = (y & 7) * 32;
  const int a = n[iy][ix] + (((n[iy][ix + 1] - n[iy][ix]) * tx) >> 8);
  const int b = n[iy + 1][ix] + (((n[iy + 1][ix + 1] - n[iy + 1][ix]) * tx) >> 8);
  return a + (((b - a) * ty) >> 8);
}

void setupSky() {
  Sky& s = *reinterpret_cast<Sky*>(mem);
  if (night) {
    gradient(rgb(0, 0, 0), rgb(2, 2, 10), rgb(6, 3, 14), 140);
  } else {
    gradient(rgb(2, 4, 18), rgb(8, 12, 38), rgb(30, 18, 54), 150);
  }
  s.slope = 0.25f + rndF() * 0.35f;
  s.offset = rndIn(-40, 60);
  // The milky way's brightness on an 8 px grid (a band with noise), read
  // bilinearly per pixel.
  for (int iy = 0; iy < kNoiseH; ++iy) {
    for (int ix = 0; ix < kNoiseW; ++ix) {
      const float d = (iy * 8 - s.slope * ix * 8 - s.offset) / 34.0f;
      const int n = rnd() & 0xFF;
      s.noise[iy][ix] = static_cast<uint8_t>(min(255.0f, expf(-d * d) * (40 + n / 4)));
    }
  }
  makeStars(s.stars, 120, H - 1, 18);
}

void rowSky(int16_t y, int16_t x0, int16_t w, uint16_t* out) {
  const Sky& s = *reinterpret_cast<const Sky*>(mem);
  constexpr uint16_t kMilky = rgb(90, 96, 150);
  for (int i = 0; i < w; ++i) {
    const int x = x0 + i;
    out[i] = swap16(blend(baseAt(x, y), kMilky, noiseAt(s.noise, x, y) * dim >> 8));
  }
  paintStars(s.stars, y, x0, w, out);
}

// ---- Aurora: three flowing ribbons with rays ----

struct Aurora {
  int16_t centre[3][W];  // ribbon's lower edge, 1/4 px
  uint16_t colour[3][W];
  uint8_t ray[3][W];
  uint8_t up[64], down[16];
};
constexpr int kAuroraUp[3] = {60, 48, 38};
constexpr int kAuroraUpInv[3] = {64 * 65536 / 60 + 1, 64 * 65536 / 48 + 1, 64 * 65536 / 38 + 1};

void setupAurora() {
  Aurora& a = *reinterpret_cast<Aurora*>(mem);
  if (night) {
    gradient(rgb(0, 0, 0), rgb(0, 2, 6), rgb(2, 4, 10), 120);
  } else {
    gradient(rgb(2, 5, 14), rgb(3, 10, 24), rgb(8, 10, 30), 120);
  }
  static const uint16_t kFrom[3] = {rgb(60, 240, 150), rgb(40, 190, 235), rgb(110, 245, 130)};
  static const uint16_t kTo[3] = {rgb(40, 205, 215), rgb(150, 95, 245), rgb(230, 100, 210)};
  static const int kBaseY[3] = {96, 128, 160};
  for (int r = 0; r < 3; ++r) {
    const float f1 = 0.008f + rndF() * 0.010f, p1 = rndF() * 6.28f, f2 = 0.025f + rndF() * 0.02f,
                p2 = rndF() * 6.28f, f3 = 0.012f + rndF() * 0.01f, p3 = rndF() * 6.28f;
    const float amp = 16 + rndF() * 14;
    const float rf = 0.15f + rndF() * 0.12f, rp = rndF() * 6.28f;
    for (int x = 0; x < W; ++x) {
      const float yc = kBaseY[r] + amp * sinf(f1 * x + p1) + 6 * sinf(f2 * x + p2);
      a.centre[r][x] = static_cast<int16_t>(yc * 4);
      a.colour[r][x] = blend(kFrom[r], kTo[r], static_cast<int>(128 + 127 * sinf(f3 * x + p3)));
      const float ray = 0.5f + 0.5f * (0.5f + 0.5f * sinf(rf * x + rp)) * (0.6f + 0.4f * sinf(0.043f * x + p1));
      a.ray[r][x] = static_cast<uint8_t>(255 * ray);
    }
  }
  for (int d = 0; d < 64; ++d) {
    const float t = 1 - d / 64.0f;
    a.up[d] = static_cast<uint8_t>(255 * t * t);
  }
  for (int d = 0; d < 16; ++d) {
    const float t = 1 - d / 16.0f;
    a.down[d] = static_cast<uint8_t>(255 * t * t * t);
  }
}

void rowAurora(int16_t y, int16_t x0, int16_t w, uint16_t* out) {
  const Aurora& a = *reinterpret_cast<const Aurora*>(mem);
  const int strength = 125 * dim >> 8;
  const int y4 = y * 4;
  for (int i = 0; i < w; ++i) {
    const int x = x0 + i;
    uint16_t c = baseAt(x, y);
    for (int r = 0; r < 3; ++r) {
      const int dy = y4 - a.centre[r][x];  // < 0: above the lower edge
      int v;
      if (dy < 0) {
        const int d = ((-dy >> 2) * kAuroraUpInv[r]) >> 16;  // (-dy/4) * 64 / kAuroraUp[r]
        if (d >= 64) continue;
        v = a.up[d];
      } else {
        const int d = dy >> 2;
        if (d >= 16) continue;
        v = a.down[d];
      }
      c = blend(c, a.colour[r][x], (v * a.ray[r][x] >> 8) * strength >> 8);
    }
    out[i] = swap16(c);
  }
}

// ---- Bokeh: blurred discs of light ----

struct Disc {
  int16_t x, y, r;
  uint16_t colour;
  uint8_t alpha;
};
struct Bokeh {
  int count;
  Disc disc[16];
};

void setupBokeh() {
  Bokeh& b = *reinterpret_cast<Bokeh*>(mem);
  gradient(blend(base, acc1, 18), base, blend(base, acc2, 34), 110);
  if (night) gradient(rgb(0, 0, 0), rgb(0, 0, 0), blend(rgb(0, 0, 0), acc2, 30), 110);
  const uint16_t colours[] = {acc1, acc2, rgb(255, 176, 70), rgb(255, 96, 160), rgb(70, 205, 230),
                              blend(acc1, rgb(255, 255, 255), 90)};
  b.count = 15;
  for (int i = 0; i < b.count; ++i) {  // one per cell of a 5x3 grid, so they spread out
    Disc& d = b.disc[i];
    const float s = rndF();
    d.r = static_cast<int16_t>(10 + s * s * 36);
    d.x = (i % 5) * 64 + rndIn(-6, 70);
    d.y = (i / 5) * 80 + rndIn(0, 80);
    d.colour = colours[rnd() % 6];
    d.alpha = static_cast<uint8_t>(rndIn(28, 70) * (d.r < 20 ? 13 : 10) / 10);
  }
}

void rowBokeh(int16_t y, int16_t x0, int16_t w, uint16_t* out) {
  const Bokeh& b = *reinterpret_cast<const Bokeh*>(mem);
  for (int i = 0; i < w; ++i) out[i] = swap16(baseAt(x0 + i, y));
  for (int k = 0; k < b.count; ++k) {
    const Disc& d = b.disc[k];
    const int dy = y - d.y;
    if (dy <= -d.r || dy >= d.r) continue;
    const float hw = sqrtf(static_cast<float>(d.r * d.r - dy * dy));
    const int xa = max<int>(x0, static_cast<int>(d.x - hw) - 1), xb = min<int>(x0 + w - 1, static_cast<int>(d.x + hw) + 1);
    const int alpha = d.alpha * dim >> 8;
    for (int x = xa; x <= xb; ++x) {
      const float in = hw - fabsf(x - d.x) + 0.5f;  // px inside the edge
      if (in <= 0) continue;
      int a = in >= 1 ? alpha : static_cast<int>(alpha * in);
      if (in < 3) a = a * 13 / 10;  // a brighter rim, like a lens
      out[x - x0] = swap16(blend(swap16(out[x - x0]), d.colour, a));
    }
  }
}

// ---- Waves: layered smooth hills ----

struct Waves {
  int16_t edge[4][W];       // each layer's top edge, 1/16 px
  uint16_t colour[4][H];    // each layer's colour by row
};

void setupWaves() {
  Waves& v = *reinterpret_cast<Waves*>(mem);
  const uint16_t top = night ? rgb(0, 0, 0) : base;
  gradient(top, blend(top, acc1, 40 * dim >> 8), blend(top, acc2, 70 * dim >> 8), 150);
  static const int kBase[4] = {136, 164, 192, 218};
  static const int kAlpha[4] = {120, 96, 72, 46};
  for (int k = 0; k < 4; ++k) {
    const float f1 = 0.010f + rndF() * 0.012f, p1 = rndF() * 6.28f, f2 = 0.03f + rndF() * 0.02f,
                p2 = rndF() * 6.28f, amp = 9 + rndF() * 9 - k * 1.5f;
    for (int x = 0; x < W; ++x) {
      v.edge[k][x] = static_cast<int16_t>((kBase[k] + amp * sinf(f1 * x + p1) + 3 * sinf(f2 * x + p2)) * 16);
    }
    const uint16_t tone = blend(acc1, acc2, k * 85);
    for (int y = 0; y < H; ++y) {
      const int a = kAlpha[k] * dim >> 8;
      const uint16_t c = blend(top, tone, a);
      v.colour[k][y] = blend(c, top, max(0, (y - kBase[k]) * 3));  // darker going down
    }
  }
}

void rowWaves(int16_t y, int16_t x0, int16_t w, uint16_t* out) {
  const Waves& v = *reinterpret_cast<const Waves*>(mem);
  const int y16 = y * 16 + 8;
  for (int i = 0; i < w; ++i) {
    const int x = x0 + i;
    uint16_t c = baseAt(x, y);
    for (int k = 0; k < 4; ++k) {
      const int d = y16 - v.edge[k][x];
      if (d <= -8) continue;
      c = blend(c, v.colour[k][y], d >= 8 ? 256 : (d + 8) * 16);  // anti-aliased edge
    }
    out[i] = swap16(c);
  }
}

// ---- Synthwave: a striped sun on a neon horizon grid ----

constexpr int kHorizon = 144;  // just above the minute bar (y 150)
struct Synth {
  Stars stars;
  uint8_t hline[H];  // horizontal grid lines by row
};

void setupSynth() {
  Synth& s = *reinterpret_cast<Synth*>(mem);
  if (night) {
    gradient(rgb(0, 0, 0), rgb(16, 2, 18), rgb(2, 0, 6), kHorizon);
  } else {
    gradient(rgb(8, 3, 26), rgb(84, 16, 86), rgb(10, 3, 26), kHorizon);
  }
  memset(s.hline, 0, sizeof(s.hline));
  for (int k = 1;; ++k) {
    const float y = kHorizon + 2.5f * k + 0.85f * k * k;
    if (y >= H) break;
    const int iy = static_cast<int>(y);
    const float f = y - iy;
    s.hline[iy] = max<int>(s.hline[iy], static_cast<int>(230 * (1 - f)));
    if (iy + 1 < H) s.hline[iy + 1] = max<int>(s.hline[iy + 1], static_cast<int>(230 * f));
  }
  makeStars(s.stars, 45, kHorizon - 60, 10);
}

void rowSynth(int16_t y, int16_t x0, int16_t w, uint16_t* out) {
  const Synth& s = *reinterpret_cast<const Synth*>(mem);
  constexpr uint16_t kSunTop = rgb(255, 214, 90), kSunBottom = rgb(255, 54, 150);
  constexpr uint16_t kGrid = rgb(235, 70, 230), kHaze = rgb(255, 80, 200);
  constexpr int kSunX = 160, kSunR = 46;
  const int sunA = 120 * dim >> 8, gridA = 150 * dim >> 8;
  // The sun's horizontal cuts: thicker towards the horizon.
  const int below = y - (kHorizon - kSunR);
  bool cut = false;
  if (below > kSunR / 2) {
    const int k = (below - kSunR / 2);
    cut = (k % 9) < 1 + k / 8;
  }
  const float sdy = static_cast<float>(kHorizon - y);
  const uint16_t sun = blend(kSunTop, kSunBottom, max(0, below) * 256 / kSunR);
  const float spacing = y > kHorizon ? 54.0f * (y - kHorizon) / (H - kHorizon) : 0;
  const float invSpacing = spacing > 0 ? 1 / spacing : 0;
  const int fade = y > kHorizon ? (y - kHorizon) * 256 / (H - kHorizon) : 0;
  for (int i = 0; i < w; ++i) {
    const int x = x0 + i;
    uint16_t c = baseAt(x, y);
    if (y < kHorizon) {
      const int glow = abs(y - kHorizon) < 24 ? (24 - abs(y - kHorizon)) * 4 : 0;  // haze over the horizon
      c = blend(c, kHaze, glow * dim >> 8);
      if (!cut && sdy < kSunR + 1 && abs(x - kSunX) <= kSunR + 1) {
        const float sdx = static_cast<float>(x - kSunX);
        const float in = kSunR - sqrtf(sdx * sdx + sdy * sdy) + 0.5f;  // anti-aliased disc
        if (in > 0) c = blend(c, sun, in >= 1 ? sunA : static_cast<int>(sunA * in));
      }
    } else if (y == kHorizon) {
      c = blend(c, kGrid, 220 * dim >> 8);
    } else {
      int a = s.hline[y] * fade >> 8;
      const float u = (x - kSunX) * invSpacing;
      const float dist = fabsf(u - static_cast<float>(static_cast<int>(u + (u < 0 ? -0.5f : 0.5f)))) * spacing;
      // Fade the converging lines where they crowd together near the horizon.
      const int crowd = spacing < 6 ? static_cast<int>(spacing * 256 / 6) : 256;
      if (dist < 1.3f) a = max(a, (static_cast<int>((1.3f - dist) / 1.3f * 255) * (fade + 60) / 316) * crowd >> 8);
      c = blend(c, kGrid, a * gridA >> 8);
    }
    out[i] = swap16(c);
  }
  paintStars(s.stars, y, x0, w, out);
}

// ---- Photo: a darkened, blurred copy of a stored photo ----

constexpr int kPhotoW = 80, kPhotoH = 60;  // any photo, centre-cropped to 4:3, sampled
struct Photo {
  uint16_t px[kPhotoH][kPhotoW];
};
Photo* decoding = nullptr;
int16_t sampleX[kPhotoW], sampleY[kPhotoH];  // the decoded pixel each cell takes

bool setupPhoto(const char* path) {
  Photo& p = *reinterpret_cast<Photo*>(mem);
  memset(&p, 0, sizeof(p));
  if (!path || !LittleFS.exists(path)) return false;
  uint16_t jw = 0, jh = 0;
  if (TJpgDec.getFsJpgSize(&jw, &jh, path, LittleFS) != JDR_OK || !jw || !jh) return false;
  // The smallest decode that still has at least 80x60 pixels (as fast as possible).
  uint8_t scale = 8;
  while (scale > 1 && (jw / scale < kPhotoW || jh / scale < kPhotoH)) scale /= 2;
  const int dw = max(1, jw / scale), dh = max(1, jh / scale);
  // Centre-crop to 4:3, then one sample per cell.
  int cw = dw, ch = dh;
  if (cw * 3 > ch * 4) cw = ch * 4 / 3; else ch = cw * 3 / 4;
  cw = max(cw, 1);
  ch = max(ch, 1);
  for (int i = 0; i < kPhotoW; ++i) sampleX[i] = (dw - cw) / 2 + (2 * i + 1) * cw / (2 * kPhotoW);
  for (int i = 0; i < kPhotoH; ++i) sampleY[i] = (dh - ch) / 2 + (2 * i + 1) * ch / (2 * kPhotoH);
  decoding = &p;
  TJpgDec.setJpgScale(scale);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback([](int16_t x, int16_t y, uint16_t bw, uint16_t bh, uint16_t* bitmap) {
    for (int r = 0; r < kPhotoH; ++r) {
      const int sy = sampleY[r];
      if (sy < y || sy >= y + bh) continue;
      for (int c = 0; c < kPhotoW; ++c) {
        const int sx = sampleX[c];
        if (sx >= x && sx < x + bw) decoding->px[r][c] = bitmap[(sy - y) * bw + (sx - x)];
      }
    }
    return y <= sampleY[kPhotoH - 1];  // the rows below aren't needed: stop decoding
  });
  decoding = nullptr;
  if (r != JDR_OK && r != JDR_INTR) return false;
  // Blur (a 3x3 box, three times) and darken, with a vignette.
  auto* tmp = static_cast<uint16_t(*)[kPhotoW]>(malloc(sizeof(uint16_t) * kPhotoW * kPhotoH));
  if (!tmp) return false;
  for (int pass = 0; pass < 3; ++pass) {
    for (int y = 0; y < kPhotoH; ++y) {
      for (int x = 0; x < kPhotoW; ++x) {
        int r = 0, g = 0, b = 0, n = 0;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const int yy = constrain(y + dy, 0, kPhotoH - 1), xx = constrain(x + dx, 0, kPhotoW - 1);
            const uint16_t c = p.px[yy][xx];
            r += c >> 11;
            g += (c >> 5) & 63;
            b += c & 31;
            ++n;
          }
        }
        tmp[y][x] = ((r / n) << 11) | ((g / n) << 5) | (b / n);
      }
    }
    memcpy(p.px, tmp, sizeof(uint16_t) * kPhotoW * kPhotoH);
  }
  free(tmp);
  const int keep = night ? 60 : 112;
  for (int y = 0; y < kPhotoH; ++y) {
    for (int x = 0; x < kPhotoW; ++x) {
      const float dx = (x - 39.5f) / 40, dy = (y - 29.5f) / 30;
      const int vignette = static_cast<int>(keep * (1 - 0.45f * (dx * dx + dy * dy) / 2));
      p.px[y][x] = blend(night ? rgb(0, 0, 0) : base, p.px[y][x], vignette);
    }
  }
  gradient(base, base, base, 120);
  return true;
}

void rowPhoto(int16_t y, int16_t x0, int16_t w, uint16_t* out) {
  const Photo& p = *reinterpret_cast<const Photo*>(mem);
  const int fy = max(0, y * 64 - 96);  // 1/256 px in the 80x60 copy, pixel centres aligned
  const int iy = min(fy >> 8, kPhotoH - 2), ty = fy >> 8 >= kPhotoH - 1 ? 256 : fy & 255;
  for (int i = 0; i < w; ++i) {
    const int fx = max(0, (x0 + i) * 64 - 96);
    const int ix = min(fx >> 8, kPhotoW - 2), tx = fx >> 8 >= kPhotoW - 1 ? 256 : fx & 255;
    const uint16_t a = blend(p.px[iy][ix], p.px[iy][ix + 1], tx);
    const uint16_t b = blend(p.px[iy + 1][ix], p.px[iy + 1][ix + 1], tx);
    out[i] = swap16(blend(a, b, ty));
  }
}

// ---- Shooting stars (Night sky, by day) ----

struct Meteor {
  uint32_t startMs = 0, nextMs = 0;
  float x0 = 0, y0 = 0, dx = 0, dy = 0;
  bool on = false;
  int lit = 0;
  Spark pix[24];  // lit pixels: x, y, to = colour shown, from = background under it
};
Meteor meteor;
constexpr uint32_t kMeteorMs = 650;

size_t memFor(Theme t) {
  switch (t) {
    case Theme::Glow: return sizeof(Glow);
    case Theme::Sky: return sizeof(Sky);
    case Theme::Aurora: return sizeof(Aurora);
    case Theme::Bokeh: return sizeof(Bokeh);
    case Theme::Waves: return sizeof(Waves);
    case Theme::Synthwave: return sizeof(Synth);
    case Theme::Photo: return sizeof(Photo);
    case Theme::None: return 0;
  }
  return 0;
}

const char* const kNames[kThemeCount] = {"none", "glow", "sky", "aurora", "bokeh", "waves", "synthwave", "photo"};
const char* const kTitles[kThemeCount] = {"None",  "Glow",  "Night sky", "Aurora",
                                          "Bokeh", "Waves", "Synthwave", "Photo"};

}  // namespace

const char* name(Theme t) { return kNames[static_cast<int>(t)]; }
const char* title(Theme t) { return kTitles[static_cast<int>(t)]; }

bool fromName(const char* n, Theme& out) {
  if (!strcasecmp(n, "off")) n = "none";
  for (int i = 0; i < kThemeCount; ++i) {
    if (!strcasecmp(n, kNames[i]) || !strcasecmp(n, kTitles[i])) {
      out = static_cast<Theme>(i);
      return true;
    }
  }
  return false;
}

void end() {
  free(mem);
  mem = nullptr;
  theme = Theme::None;
  meteor = Meteor();
}

bool setup(Theme t, uint16_t bg, uint16_t accent, uint16_t accent2, bool isNight, uint32_t seed,
           const char* photoPath) {
  end();
  base = bg;
  night = isNight;
  dim = isNight ? 120 : 256;
  acc1 = accent;
  acc2 = accent2;
  rng = seed ? seed : 1;
  if (t == Theme::None) return true;
  mem = static_cast<uint8_t*>(malloc(memFor(t)));
  if (!mem) return false;
  theme = t;
  bool ok = true;
  switch (t) {
    case Theme::Glow: setupGlow(); break;
    case Theme::Sky: setupSky(); break;
    case Theme::Aurora: setupAurora(); break;
    case Theme::Bokeh: setupBokeh(); break;
    case Theme::Waves: setupWaves(); break;
    case Theme::Synthwave: setupSynth(); break;
    case Theme::Photo: ok = setupPhoto(photoPath); break;
    case Theme::None: break;
  }
  if (!ok) end();
  meteor.nextMs = millis() + 8000;
  return ok;
}

bool active() { return mem != nullptr; }
Theme current() { return theme; }

void row(int16_t y, int16_t x0, int16_t w, uint16_t* out) {
  if (w <= 0) return;
  if (y < 0 || y >= H || !mem) {
    const uint16_t c = swap16(base);
    for (int i = 0; i < w; ++i) out[i] = c;
    return;
  }
  // Columns off the screen (a sprite past its edge) take the edge's colour.
  int16_t skip = 0;
  if (x0 < 0) {
    skip = min<int16_t>(-x0, w);
    x0 = 0;
  }
  const int16_t n = min<int16_t>(w - skip, W - x0);
  uint16_t* o = out + skip;
  if (n > 0) {
    switch (theme) {
      case Theme::Glow: rowGlow(y, x0, n, o); break;
      case Theme::Sky: rowSky(y, x0, n, o); break;
      case Theme::Aurora: rowAurora(y, x0, n, o); break;
      case Theme::Bokeh: rowBokeh(y, x0, n, o); break;
      case Theme::Waves: rowWaves(y, x0, n, o); break;
      case Theme::Synthwave: rowSynth(y, x0, n, o); break;
      case Theme::Photo: rowPhoto(y, x0, n, o); break;
      case Theme::None: break;
    }
  }
  for (int i = 0; i < skip; ++i) out[i] = n > 0 ? o[0] : swap16(base);
  for (int i = skip + max<int16_t>(n, 0); i < w; ++i) out[i] = i > 0 ? out[i - 1] : swap16(base);
}

uint16_t at(int16_t x, int16_t y) {
  uint16_t c;
  row(y, x, 1, &c);
  return swap16(c);
}

int sparkles(uint32_t now, Spark* out, int max) {
  if (!mem || (theme != Theme::Sky && theme != Theme::Synthwave)) return 0;
  Stars& stars = theme == Theme::Sky ? reinterpret_cast<Sky*>(mem)->stars : reinterpret_cast<Synth*>(mem)->stars;
  int n = 0;
  for (int i = 0; i < stars.count && n < max; ++i) {
    Star& st = stars.star[i];
    if (!st.twinkles) continue;
    const float ph = ((now + st.phase) % st.periodMs) / static_cast<float>(st.periodMs);
    const int b = static_cast<int>(st.bright * (0.3f + 0.7f * (0.5f + 0.5f * sinf(ph * 2 * PI))));
    const uint8_t q = static_cast<uint8_t>(b & ~7);  // 32 steps: fewer, larger changes
    if (q == st.cur) continue;
    const uint16_t from = at(st.x, st.y);
    st.cur = q;
    out[n++] = {st.x, static_cast<int16_t>(st.y), from, at(st.x, st.y)};
  }
  if (theme != Theme::Sky || night) return n;
  // A shooting star now and then: a short streak down the sky.
  Meteor& m = meteor;
  if (!m.on && static_cast<int32_t>(now - m.nextMs) >= 0) {
    m.on = true;
    m.startMs = now;
    m.x0 = rndIn(60, W - 20);
    m.y0 = rndIn(4, 70);
    const float a = (200 + rndIn(0, 30)) * PI / 180;  // down and to the left
    m.dx = cosf(a) * 260;
    m.dy = -sinf(a) * 260;
  }
  Spark next[24];
  int lit = 0;
  if (m.on) {
    const float t = (now - m.startMs) / static_cast<float>(kMeteorMs);
    if (t >= 1) {
      m.on = false;
      m.nextMs = now + rndIn(20000, 55000);
    } else {
      const float hx = m.x0 + m.dx * t * 0.35f, hy = m.y0 + m.dy * t * 0.35f;
      const float len = sqrtf(m.dx * m.dx + m.dy * m.dy);
      const float fade = t < 0.75f ? 1 : (1 - t) * 4;
      for (int k = 0; k < 24 && lit < 24; ++k) {
        const int x = static_cast<int>(hx - m.dx / len * k * 1.3f), y = static_cast<int>(hy - m.dy / len * k * 1.3f);
        if (x < 0 || x >= W || y < 0 || y >= H) continue;
        bool dup = false;
        for (int j = 0; j < lit; ++j) dup |= next[j].x == x && next[j].y == y;
        if (dup) continue;
        const uint16_t under = at(x, y);
        next[lit++] = {static_cast<int16_t>(x), static_cast<int16_t>(y), under,
                       blend(under, rgb(255, 255, 255), static_cast<int>((24 - k) * 10 * fade))};
      }
    }
  }
  // Pixels no longer lit go back to the background; new or changed ones light up.
  for (int j = 0; j < m.lit && n < max; ++j) {
    bool still = false;
    for (int k = 0; k < lit; ++k) still |= next[k].x == m.pix[j].x && next[k].y == m.pix[j].y;
    if (!still) out[n++] = {m.pix[j].x, m.pix[j].y, m.pix[j].to, m.pix[j].from};
  }
  for (int k = 0; k < lit && n < max; ++k) {
    uint16_t shown = next[k].from;
    for (int j = 0; j < m.lit; ++j) {
      if (m.pix[j].x == next[k].x && m.pix[j].y == next[k].y) shown = m.pix[j].to;
    }
    if (shown != next[k].to) out[n++] = {next[k].x, next[k].y, shown, next[k].to};
  }
  memcpy(m.pix, next, sizeof(Spark) * lit);
  m.lit = lit;
  return n;
}

}  // namespace backdrop
