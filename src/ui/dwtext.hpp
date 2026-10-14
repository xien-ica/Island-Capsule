#pragma once
// DirectWrite text for the capsule. GDI+ DrawString renders soft, uneven grayscale text on the translucent
// layered surface; DirectWrite (through a Direct2D DC render target, premultiplied alpha) renders far crisper text.
//
// Everything else in the capsule stays GDI+: each string is rendered ONCE into a cached premultiplied bitmap
// (opaque ink colour, no alpha) and then composited with Gdiplus::Graphics::DrawImage, so the existing clips,
// transforms, cross-fades and marquee strips keep working unchanged. Fade amounts are applied at draw time.
//
// UI thread only. Every failure is soft: Init() / Get() return false / nullptr and the caller falls back to GDI+.

#include "ui/gdip.hpp"

#include <memory>
#include <string>

namespace island {

class DwText {
public:
  struct Run {
    Gdiplus::Bitmap* bmp = nullptr;   // 32bpp premultiplied, owned by the cache (valid until the next Get())
    int w = 0, h = 0;                 // pixels
  };

  DwText();
  ~DwText();
  DwText(const DwText&) = delete;
  DwText& operator=(const DwText&) = delete;

  // title_px: bold, artist_px: regular, both whole pixels. False => DirectWrite unavailable (use GDI+).
  // family: requested font name; falls back to Segoe UI Variable Text, Segoe UI, Arial when not installed.
  bool Init(float title_px, float artist_px, const std::wstring& family);

  float Width(const std::wstring& s, bool artist);   // advance width, pixels
  float LineHeight(bool artist);                     // whole pixels

  // Cached render of `s` in the title (artist=false) or artist font, in an opaque colour.
  const Run* Get(const std::wstring& s, bool artist, int r, int g, int b);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace island
