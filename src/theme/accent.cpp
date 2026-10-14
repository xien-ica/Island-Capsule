#include "theme/accent.hpp"

#include <algorithm>
#include <cmath>

namespace island {

namespace {

constexpr int kBins = 24;

void ToHsv(float r, float g, float b, float* h, float* s, float* v) {
  const float mx = (std::max)({ r, g, b });
  const float mn = (std::min)({ r, g, b });
  const float d = mx - mn;
  *v = mx;
  *s = mx <= 0.0f ? 0.0f : d / mx;
  if (d <= 1e-6f) { *h = 0.0f; return; }
  float hh;
  if (mx == r)      hh = std::fmod((g - b) / d, 6.0f);
  else if (mx == g) hh = (b - r) / d + 2.0f;
  else              hh = (r - g) / d + 4.0f;
  hh *= 60.0f;
  if (hh < 0.0f) hh += 360.0f;
  *h = hh;
}

Rgb FromHsv(float h, float s, float v) {
  const float c = v * s;
  const float hp = std::fmod(h, 360.0f) / 60.0f;
  const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
  float r = 0, g = 0, b = 0;
  if (hp < 1)      { r = c; g = x; }
  else if (hp < 2) { r = x; g = c; }
  else if (hp < 3) { g = c; b = x; }
  else if (hp < 4) { g = x; b = c; }
  else if (hp < 5) { r = x; b = c; }
  else             { r = c; b = x; }
  const float m = v - c;
  return { (r + m) * 255.0f, (g + m) * 255.0f, (b + m) * 255.0f };
}

} // namespace

namespace {
// pow(v / 255, 2.2) for v = 0..255, built once. Luma() used to call pow() three times per call.
const float* LinTable() {
  static const struct Table {
    float v[256];
    Table() {
      for (int i = 0; i < 256; ++i) v[i] = std::pow(static_cast<float>(i) / 255.0f, 2.2f);
    }
  } t;
  return t.v;
}

// Linear interpolation between table entries: the channels are continuous floats (they ease), not bytes.
inline float Lin(float v, const float* t) {
  v = (std::clamp)(v, 0.0f, 255.0f);
  const int i = (std::min)(254, static_cast<int>(v));
  const float f = v - static_cast<float>(i);
  return t[i] + (t[i + 1] - t[i]) * f;
}
} // namespace

float Luma(const Rgb& c) {
  const float* t = LinTable();
  return 0.2126f * Lin(c.r, t) + 0.7152f * Lin(c.g, t) + 0.0722f * Lin(c.b, t);
}

Accent ExtractAccent(const ArtImage& art) {
  Accent out;
  if (art.w <= 0 || art.h <= 0 ||
      art.bgra.size() != static_cast<size_t>(art.w) * static_cast<size_t>(art.h) * 4)
    return out;

  double bw[kBins] = {};
  double br[kBins] = {}, bg[kBins] = {}, bb[kBins] = {};
  double avg_r = 0, avg_g = 0, avg_b = 0, total_w = 0;
  int count = 0;

  const int step = (std::max)(1, (std::min)(art.w, art.h) / 48);
  for (int y = 0; y < art.h; y += step) {
    for (int x = 0; x < art.w; x += step) {
      const uint8_t* p = &art.bgra[(static_cast<size_t>(y) * art.w + x) * 4];
      const int a = p[3];
      if (a < 128) continue;
      // un-premultiply
      const float r = (std::min)(255.0f, p[2] * 255.0f / a);
      const float g = (std::min)(255.0f, p[1] * 255.0f / a);
      const float b = (std::min)(255.0f, p[0] * 255.0f / a);

      float h, s, v;
      ToHsv(r / 255.0f, g / 255.0f, b / 255.0f, &h, &s, &v);

      ++count;
      avg_r += r; avg_g += g; avg_b += b;

      // Vivid + reasonably bright pixels vote; greys and near-black do not.
      if (v < 0.15f) continue;
      const double w = static_cast<double>(s) * s * v;
      if (w <= 0.0) continue;
      const int bin = (std::min)(kBins - 1, static_cast<int>(h / 360.0f * kBins));
      bw[bin] += w;
      br[bin] += r * w; bg[bin] += g * w; bb[bin] += b * w;
      total_w += w;
    }
  }
  if (count == 0) return out;

  const float ar = static_cast<float>(avg_r / count);
  const float ag = static_cast<float>(avg_g / count);
  const float ab = static_cast<float>(avg_b / count);
  const double chroma_share = total_w / count;   // 0 (grey art) .. ~0.5 (very vivid)

  // Best hue bin, with its neighbours helping (hue bins are circular).
  int best = -1;
  double best_score = 0.0;
  for (int i = 0; i < kBins; ++i) {
    const double score = bw[i] + 0.5 * (bw[(i + kBins - 1) % kBins] + bw[(i + 1) % kBins]);
    if (score > best_score) { best_score = score; best = i; }
  }

  if (best < 0 || chroma_share < 0.02) {
    // Greyscale / very dark / very pale cover: neutral wash.
    const float l = (std::clamp)((ar + ag + ab) / 3.0f, 70.0f, 200.0f);
    out.color = { l, l, l };
    out.strength = 0.30f;
    return out;
  }

  double sw = 0, sr = 0, sg = 0, sb = 0;
  for (int d = -1; d <= 1; ++d) {
    const int i = (best + d + kBins) % kBins;
    sw += bw[i]; sr += br[i]; sg += bg[i]; sb += bb[i];
  }
  Rgb c{ static_cast<float>(sr / sw), static_cast<float>(sg / sw), static_cast<float>(sb / sw) };

  // Keep it lively but never garish / muddy.
  float h, s, v;
  ToHsv(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, &h, &s, &v);
  s = (std::clamp)(s, 0.40f, 0.95f);
  v = (std::clamp)(v, 0.45f, 0.95f);
  out.color = FromHsv(h, s, v);
  out.strength = (std::clamp)(0.50f + static_cast<float>(chroma_share) * 1.2f, 0.50f, 1.0f);
  return out;
}

} // namespace island