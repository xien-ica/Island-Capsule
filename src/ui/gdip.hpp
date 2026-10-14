#pragma once
// Shared GDI+ include preamble + small helpers.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <algorithm>
// gdiplus.h uses unqualified min/max; with NOMINMAX they must come from std.
using std::min;
using std::max;
#include <gdiplus.h>

#include "theme/accent.hpp"

#pragma comment(lib, "gdiplus.lib")

namespace island {

inline Gdiplus::Color Col(int a, int r, int g, int b) {
  return Gdiplus::Color(static_cast<BYTE>(a), static_cast<BYTE>(r),
                        static_cast<BYTE>(g), static_cast<BYTE>(b));
}

// base alpha (0-255) scaled by a 0-1 multiplier
inline int ScaleA(int base, float mul) {
  return static_cast<int>(base * (std::clamp)(mul, 0.0f, 1.0f) + 0.5f);
}

// Phase 3: theme color (0..255 floats) + alpha (0..255 float) * multiplier (0..1)
inline Gdiplus::Color Col(const Rgb& c, float a, float mul = 1.0f) {
  auto ch = [](float v) { return static_cast<int>((std::clamp)(v, 0.0f, 255.0f) + 0.5f); };
  return Col(ScaleA(ch(a), mul), ch(c.r), ch(c.g), ch(c.b));
}

// Capsule (radius = h / 2)
inline void AddPill(Gdiplus::GraphicsPath& p, float x, float y, float w, float h) {
  const float d = h;
  p.AddArc(x, y, d, d, 90.0f, 180.0f);
  p.AddArc(x + w - d, y, d, d, 270.0f, 180.0f);
  p.CloseFigure();
}

inline void AddRoundRect(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
  const float d = r * 2.0f;
  p.AddArc(x, y, d, d, 180.0f, 90.0f);
  p.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
  p.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
  p.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
  p.CloseFigure();
}

} // namespace island
