#pragma once
// Phase 4 – the in-pill mini visualizer (the signature element).
//
// Time-based "fake reactive" motion: per-band sines + smooth noise + a beat pulse, seeded
// per track so every song feels a little different. No audio capture - stable inside
// Explorer-adjacent processes. A real WASAPI/FFT source can later replace Update()'s band
// synthesis without touching Draw() (see SetBands()).
//
// States:
//   playing  -> bands animate (spring-up from the paused state, slight overshoot)
//   paused   -> "living pause": the viz collapses to two dots
//   no media -> two dots, dimmed
//
// Styles (settings: visualizer=ticks|wave|glow|off):
//   Ticks (V1)  6 rounded vertical bars
//   Wave  (V2)  one smooth filled ribbon
//   Glow  (V4)  soft accent bloom that breathes with the bass

#include <cstdint>
#include <memory>

#include "audio/spectrum.hpp"
#include "settings/settings.hpp"
#include "ui/gdip.hpp"
#include "ui/motion.hpp"

namespace island {

class Visualizer {
public:
  static constexpr int kBands = 6;

  void SetStyle(VizStyle s) { style_ = s; }
  VizStyle Style() const { return style_; }
  bool Enabled() const { return style_ != VizStyle::Off; }

  void Layout(double scale, int pill_h);
  int SlotWidth() const { return slot_w_; }
  int SlotHeight() const { return slot_h_; }

  void SetState(bool has_session, bool playing);
  void SetSeed(uint32_t seed);               // per track: tempo / phases / noise offset
  void SetReducedMotion(bool v) { reduced_ = v; }

  // Hook to push bands by hand: bypass the synthesis for one frame.
  void SetBands(const float (&bands)[kBands]);

  // Real audio (WASAPI loopback + FFT, src/audio/spectrum.*). While the source reports Live the bands follow the
  // music; in any other state (not started yet, no device) the synthetic motion below runs instead. Pass nullptr
  // to force the synthetic motion. The pointer is not owned.
  void SetAudioSource(const AudioSpectrum* src) { audio_ = src; }

  // Advances the animation. Returns true if anything visible changed.
  bool Update(double dt);

  // Draws into the slot whose top-left is (x, y). `alpha` is 0..255.
  void Draw(Gdiplus::Graphics& g, float x, float y, const Rgb& color, float alpha) const;

  // Frees the cached GDI+ objects (must be called before GdiplusShutdown).
  void ReleaseGdi() { bar_path_.reset(); bar_brush_.reset(); }

private:
  void UpdateSynthetic(double dt);
  void DrawTicks(Gdiplus::Graphics& g, float x, float y, const Rgb& c, float a, float live) const;
  void DrawWave(Gdiplus::Graphics& g, float x, float y, const Rgb& c, float a, float live) const;
  void DrawGlow(Gdiplus::Graphics& g, float x, float y, const Rgb& c, float a, float live) const;
  void DrawDots(Gdiplus::Graphics& g, float x, float y, const Rgb& c, float a) const;

  const AudioSpectrum* audio_ = nullptr;

  VizStyle style_ = VizStyle::Ticks;
  bool reduced_ = false;
  bool has_session_ = false;
  bool playing_ = false;

  double scale_ = 1.0;
  int slot_w_ = 28;
  int slot_h_ = 18;

  uint32_t seed_ = 1;
  float seed_phase_ = 0.0f;
  double time_ = 0.0;
  float bands_[kBands] = { 0.2f, 0.2f, 0.2f, 0.2f, 0.2f, 0.2f };

  // Scratch objects reused by every Ticks frame (6 bars x 30 fps used to allocate a path + brush each).
  mutable std::unique_ptr<Gdiplus::GraphicsPath> bar_path_;
  mutable std::unique_ptr<Gdiplus::SolidBrush> bar_brush_;

  Spring live_;          // 0 = collapsed dots, 1 = full viz (springs, may overshoot)
  float dim_ = 0.5f;     // 1 with a session, ~0.5 without
};

} // namespace island
