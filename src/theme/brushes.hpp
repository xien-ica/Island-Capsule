#pragma once
// Phase 3 – glass surface + adaptive text colors, derived from the album accent.
// One surface language: neutral translucent base + a low-alpha accent wash + a rim.

#include "theme/accent.hpp"

namespace island {

struct GlassStyle {
  Rgb fill;        float fill_a  = 0;   // pill, art-side end (alpha 0..255)
  Rgb fill2;       float fill2_a = 0;   // pill, far end / transport strip (calmer wash)
  Rgb rim;         float rim_a   = 0;
  Rgb title;       float title_a = 0;
  Rgb sub;         float sub_a   = 0;
  Rgb ink;                              // glyphs, hover discs, art stroke, placeholder
  Rgb viz;         float viz_a   = 0;   // Phase 4: visualizer / progress / seam glow color
  float backdrop_luma = 0;              // luma of fill over the assumed taskbar
};

// `accent` / `strength` may be mid-crossfade values; the result is continuous in both.
GlassStyle ComposeGlass(bool dark_taskbar, const Rgb& accent, float strength);

} // namespace island
