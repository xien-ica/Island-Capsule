#pragma once
// Phase 3 – album accent extraction. Plain data only (no WinRT / GDI+ types).

#include "media/media_info.hpp"

namespace island {

// Channels are 0..255 floats so colors can be eased/mixed without rounding drift.
struct Rgb {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
};

inline Rgb Mix(const Rgb& a, const Rgb& b, float t) {
  return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

// Approximate relative luminance, 0 (black) .. 1 (white).
float Luma(const Rgb& c);

struct Accent {
  Rgb color{ 150.0f, 150.0f, 160.0f };   // neutral until art is known
  float strength = 0.0f;                  // 0 = no tint, 1 = full album wash
};

// Dominant *vivid* color of the cover. Greyscale / near-black covers fall back to a
// neutral tint with a low strength. ~2.3k samples. Called from the media worker thread (gsmtc.cpp) right
// after the cover is decoded; the result travels in ArtImage::accent_*.
Accent ExtractAccent(const ArtImage& art);

} // namespace island