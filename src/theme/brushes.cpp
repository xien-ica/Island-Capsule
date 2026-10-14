#include "theme/brushes.hpp"

#include <algorithm>
#include <cmath>

namespace island {

namespace {

float Smooth(float e0, float e1, float x) {
  const float t = (std::clamp)((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Dark taskbar: cap the accent's luma so white text always keeps contrast.
// Light taskbar: floor it so dark text does. (No muddy mid-grey backgrounds.)
Rgb ConstrainAccent(const Rgb& a, bool dark) {
  const float l = Luma(a);
  if (dark) {
    constexpr float kMax = 0.30f;
    if (l > kMax && l > 1e-4f) {
      const float k = std::pow(kMax / l, 1.0f / 2.2f);
      return { a.r * k, a.g * k, a.b * k };
    }
  } else {
    constexpr float kMin = 0.58f;
    if (l < kMin) {
      const float t = (std::clamp)((kMin - l) / (1.0f - l), 0.0f, 1.0f);
      return Mix(a, { 255.0f, 255.0f, 255.0f }, t);
    }
  }
  return a;
}

// Visualizer color: the album accent pushed to read against the pill (lighter on dark
// glass, deeper on light glass).
Rgb VizColor(const Rgb& accent, bool dark) {
  const float l = Luma(accent);
  if (dark) {
    constexpr float kMin = 0.30f;
    if (l < kMin) return Mix(accent, { 255.0f, 255.0f, 255.0f }, (std::clamp)((kMin - l) / (1.0f - l), 0.0f, 0.85f));
  } else {
    constexpr float kMax = 0.28f;
    if (l > kMax && l > 1e-4f) return Mix(accent, { 0.0f, 0.0f, 0.0f }, (std::clamp)((l - kMax) / l, 0.0f, 0.80f));
  }
  return accent;
}

} // namespace

GlassStyle ComposeGlass(bool dark, const Rgb& accent, float strength) {
  strength = (std::clamp)(strength, 0.0f, 1.0f);

  const Rgb base = dark ? Rgb{ 0x20, 0x20, 0x24 } : Rgb{ 0xF4, 0xF4, 0xF6 };
  const float base_a = dark ? 153.0f : 102.0f;                    // ~60 % / ~40 %
  const Rgb backdrop = dark ? Rgb{ 24, 24, 28 } : Rgb{ 235, 235, 238 };   // assumed taskbar
  const Rgb acc = ConstrainAccent(accent, dark);

  GlassStyle s;

  // Album wash over the base; the far end of the pill is calmer than the art end.
  const float wash = (dark ? 0.55f : 0.50f) * strength;
  const float bump = (dark ? 42.0f : 50.0f) * strength;           // a bit more opaque when tinted
  s.fill    = Mix(base, acc, wash);
  s.fill_a  = base_a + bump;
  s.fill2   = Mix(base, acc, wash * 0.50f);
  s.fill2_a = base_a + bump * 0.7f;

  // Adaptive text: luma of the fill composited over the taskbar decides ink.
  const Rgb eff = Mix(backdrop, s.fill, s.fill_a / 255.0f);
  s.backdrop_luma = Luma(eff);
  const float t = Smooth(0.42f, 0.58f, s.backdrop_luma);          // 0 = light ink, 1 = dark ink
  const Rgb light_ink{ 255, 255, 255 };
  const Rgb dark_ink{ 0x18, 0x18, 0x1C };
  s.ink = Mix(light_ink, dark_ink, t);
  s.title = s.ink;
  s.title_a = 240.0f - 5.0f * t;
  s.sub = s.ink;
  s.sub_a = 165.0f - 5.0f * t;

  // Rim: ink nudged toward the (unconstrained) album color, so it reads on any wallpaper.
  s.rim = Mix(s.ink, accent, 0.45f * strength);
  s.rim_a = 40.0f + 26.0f * strength;

  // Visualizer: neutral ink when there is no album tint, album accent as it fades in,
  // so it recolors with the same crossfade as the wash.
  s.viz = Mix(s.ink, VizColor(accent, dark), (std::clamp)(strength * 1.5f, 0.0f, 1.0f));
  s.viz_a = 235.0f;
  return s;
}

} // namespace island
