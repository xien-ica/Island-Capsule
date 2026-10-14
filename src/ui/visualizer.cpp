// Phase 4 – mini visualizer implementation (GDI+).

#include "ui/visualizer.hpp"

#include <cmath>
#include <memory>
#include <vector>

namespace gp = Gdiplus;

namespace island {

namespace {

constexpr float kPi = 3.14159265f;

constexpr double kSlotW = 28.0;   // logical px
constexpr double kSlotH = 18.0;

// Deterministic hash -> [0, 1)
float Hash01(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU;
  x ^= x >> 15; x *= 0x846ca68bU;
  x ^= x >> 16;
  return static_cast<float>(x & 0xFFFFFFu) / 16777216.0f;
}

// Smooth 1-D value noise in [0, 1]
float Noise(float x, uint32_t salt) {
  const float fl = std::floor(x);
  const uint32_t i = static_cast<uint32_t>(static_cast<int32_t>(fl));
  const float f = x - fl;
  const float u = f * f * (3.0f - 2.0f * f);
  const float a = Hash01(i * 2654435761u + salt);
  const float b = Hash01((i + 1) * 2654435761u + salt);
  return a + (b - a) * u;
}

float Clamp01(float v) { return (std::clamp)(v, 0.0f, 1.0f); }

} // namespace

void Visualizer::Layout(double scale, int pill_h) {
  scale_ = scale;
  slot_w_ = static_cast<int>(std::lround(kSlotW * scale));
  const int max_h = pill_h - 2 * static_cast<int>(std::lround(8.0 * scale));
  slot_h_ = (std::min)(static_cast<int>(std::lround(kSlotH * scale)), (std::max)(max_h, 6));
}

void Visualizer::SetState(bool has_session, bool playing) {
  has_session_ = has_session;
  playing_ = playing;
}

void Visualizer::SetSeed(uint32_t seed) {
  seed_ = seed ? seed : 1u;
  seed_phase_ = Hash01(seed_ ^ 0xA511E9B3u) * 6.2831853f;
}

void Visualizer::SetBands(const float (&bands)[kBands]) {
  for (int i = 0; i < kBands; ++i) bands_[i] = Clamp01(bands[i]);
}

// The original time-based motion (also the fallback when no real audio is available).
void Visualizer::UpdateSynthetic(double dt) {
  time_ += dt;
  const float t = static_cast<float>(time_);

  // Tempo is a per-track property: ~80..147 BPM.
  const float bps = 1.35f + 1.10f * Hash01(seed_ ^ 0x9E3779B9u);
  const float ph = std::fmod(t * bps, 1.0f);
  const float beat = std::exp(-ph * 4.5f);
  const float half = std::exp(-std::fmod(ph + 0.5f, 1.0f) * 7.0f) * 0.5f;
  const float energy = 0.74f + 0.26f * std::sin(t * 0.31f + seed_phase_);
  const float seed_f = static_cast<float>(seed_ & 0xFFFFu) * 0.0137f;

  for (int i = 0; i < kBands; ++i) {
    const float fi = static_cast<float>(i) / static_cast<float>(kBands - 1);   // 0 bass .. 1 treble
    const uint32_t si = seed_ + static_cast<uint32_t>(i) * 7919u;
    const float f1 = 1.7f + 2.6f * Hash01(si + 1u);
    const float f2 = 3.1f + 3.4f * Hash01(si + 5u);
    const float p = 6.2831853f * Hash01(si + 9u);

    const float s1 = 0.5f + 0.5f * std::sin(t * f1 + p);
    const float s2 = 0.5f + 0.5f * std::sin(t * f2 + p * 1.7f);
    const float n = Noise(t * (2.0f + 3.0f * fi) + static_cast<float>(i) * 17.3f + seed_f, si);
    const float pulse = (beat + half) * (0.40f + 0.60f * (1.0f - fi));   // bass leans on the beat

    float raw = 0.26f * s1 + 0.20f * s2 + 0.28f * n + 0.30f * pulse;
    raw = Clamp01(raw * energy * 1.15f);
    const float target = 0.12f + 0.88f * raw;

    // Fast attack, slow release: reads like a real analyser.
    const float rate = target > bands_[i] ? 24.0f : 7.5f;
    bands_[i] += (target - bands_[i]) * static_cast<float>(1.0 - std::exp(-dt * rate));
  }
}

bool Visualizer::Update(double dt) {
  if (!Enabled()) return false;
  bool changed = false;

  const bool active = has_session_ && playing_;
  const double k = 190.0;
  const double c = reduced_ ? CriticalDamping(k) : 13.0;   // ~2 % overshoot when springing back
  changed |= live_.Step(dt, active ? 1.0 : 0.0, k, c);

  const float dim_target = has_session_ ? 1.0f : 0.5f;
  const float dd = dim_target - dim_;
  if (std::fabs(dd) > 0.003f) {
    dim_ += dd * static_cast<float>(1.0 - std::exp(-dt * 6.0));
    changed = true;
  } else if (dd != 0.0f) {
    dim_ = dim_target;
    changed = true;
  }

  // While collapsing (paused) the bands just freeze and shrink with live_: the capture is already closed by then.
  if (active) {
    static_assert(AudioSpectrum::kBands == kBands, "audio bands must match the visualizer bands");
    if (audio_ && audio_->GetState() == AudioSpectrum::State::Live) {
      // Real audio: levels come from the FFT thread; same attack / release smoothing as the synthetic motion.
      float raw[kBands];
      audio_->GetBands(raw);
      for (int i = 0; i < kBands; ++i) {
        const float target = 0.10f + 0.90f * Clamp01(raw[i]);
        const float rate = target > bands_[i] ? 24.0f : 7.5f;
        const float next = bands_[i] + (target - bands_[i]) * static_cast<float>(1.0 - std::exp(-dt * rate));
        if (std::fabs(next - bands_[i]) > 0.0008f) changed = true;   // settled (silence) => no redraw at all
        bands_[i] = next;
      }
    } else {
      UpdateSynthetic(dt);
      changed = true;
    }
  }
  return changed;
}

void Visualizer::Draw(gp::Graphics& g, float x, float y, const Rgb& color, float alpha) const {
  if (!Enabled() || slot_w_ <= 0 || slot_h_ <= 0) return;

  const float live = (std::clamp)(static_cast<float>(live_.x), 0.0f, 1.12f);
  const float m = 1.0f - Clamp01(live);     // 0 = full viz, 1 = collapsed dots
  const float a = alpha * dim_;

  if (m < 0.995f) {
    const gp::GraphicsState st = g.Save();
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    switch (style_) {
      case VizStyle::Wave: DrawWave(g, x, y, color, a * (1.0f - m), live); break;
      case VizStyle::Glow: DrawGlow(g, x, y, color, a * (1.0f - m), live); break;
      case VizStyle::Ticks:
      default:             DrawTicks(g, x, y, color, a * (1.0f - m), live); break;
    }
    g.Restore(st);
  }
  if (m > 0.01f) DrawDots(g, x, y, color, a * m);
}

void Visualizer::DrawTicks(gp::Graphics& g, float x, float y, const Rgb& c, float a, float live) const {
  const float W = static_cast<float>(slot_w_);
  const float H = static_cast<float>(slot_h_);
  const float cy = y + H * 0.5f;
  const float bw = static_cast<float>(2.6 * scale_);
  const float gap = (W - bw * kBands) / static_cast<float>(kBands - 1);

  if (!bar_path_) bar_path_ = std::make_unique<gp::GraphicsPath>();
  if (!bar_brush_) bar_brush_ = std::make_unique<gp::SolidBrush>(gp::Color(0, 0, 0, 0));

  for (int i = 0; i < kBands; ++i) {
    const float h = (std::min)(H, (std::max)(bw, bands_[i] * live * H));
    const float bx = x + static_cast<float>(i) * (bw + gap);
    bar_path_->Reset();
    AddRoundRect(*bar_path_, bx, cy - h * 0.5f, bw, h, bw * 0.5f);
    bar_brush_->SetColor(Col(c, 255.0f, Clamp01(a / 255.0f) * (0.62f + 0.38f * bands_[i])));
    g.FillPath(bar_brush_.get(), bar_path_.get());
  }
}

void Visualizer::DrawWave(gp::Graphics& g, float x, float y, const Rgb& c, float a, float live) const {
  constexpr int N = 9;
  const float W = static_cast<float>(slot_w_);
  const float H = static_cast<float>(slot_h_);
  const float cy = y + H * 0.5f;
  const float min_amp = static_cast<float>(0.55 * scale_);

  gp::PointF top[N], bot[N];
  for (int k = 0; k < N; ++k) {
    const float u = static_cast<float>(k) / static_cast<float>(N - 1);
    const float pos = u * static_cast<float>(kBands - 1);
    const int i0 = (std::min)(static_cast<int>(pos), kBands - 1);
    const int i1 = (std::min)(i0 + 1, kBands - 1);
    const float b = bands_[i0] + (bands_[i1] - bands_[i0]) * (pos - static_cast<float>(i0));
    const float env = std::pow(std::sin(kPi * u), 0.55f);          // taper both ends
    const float full = (std::min)(H * 0.5f, b * live * H * 0.5f);
    const float amp = (std::max)(min_amp, min_amp + (full - min_amp) * env);
    const float px = x + u * W;
    top[k] = gp::PointF(px, cy - amp);
    bot[N - 1 - k] = gp::PointF(px, cy + amp);
  }

  gp::GraphicsPath ribbon;
  ribbon.AddCurve(top, N, 0.5f);
  ribbon.AddCurve(bot, N, 0.5f);
  ribbon.CloseFigure();

  const float am = Clamp01(a / 255.0f);
  gp::LinearGradientBrush fill(gp::PointF(x, 0.0f), gp::PointF(x + W, 0.0f),
                               Col(c, 245.0f, am), Col(c, 165.0f, am));
  g.FillPath(&fill, &ribbon);
}

void Visualizer::DrawGlow(gp::Graphics& g, float x, float y, const Rgb& c, float a, float live) const {
  const float W = static_cast<float>(slot_w_);
  const float H = static_cast<float>(slot_h_);
  const float cx = x + W * 0.5f;
  const float cy = y + H * 0.5f;
  const float am = Clamp01(a / 255.0f);

  // Bass-weighted energy, scaled by the spring (so it blooms back on play).
  const float e = Clamp01((bands_[0] * 0.5f + bands_[1] * 0.3f + bands_[2] * 0.2f) * live);

  const float rx = W * 0.5f * (0.40f + 0.60f * e);
  const float ry = H * 0.5f * (0.45f + 0.55f * e);

  gp::GraphicsPath halo;
  halo.AddEllipse(cx - rx * 1.25f, cy - ry * 1.25f, rx * 2.5f, ry * 2.5f);
  gp::PathGradientBrush pb(&halo);
  pb.SetCenterColor(Col(c, 255.0f, am * (0.50f + 0.50f * e)));
  gp::Color edge = Col(c, 0.0f, 0.0f);
  INT count = 1;
  pb.SetSurroundColors(&edge, &count);
  g.FillPath(&pb, &halo);

  const float core_x = rx * 0.50f, core_y = ry * 0.50f;
  gp::SolidBrush core(Col(c, 235.0f, am));
  g.FillEllipse(&core, cx - core_x, cy - core_y, core_x * 2.0f, core_y * 2.0f);
}

void Visualizer::DrawDots(gp::Graphics& g, float x, float y, const Rgb& c, float a) const {
  const float cx = x + static_cast<float>(slot_w_) * 0.5f;
  const float cy = y + static_cast<float>(slot_h_) * 0.5f;
  const float r = static_cast<float>(1.7 * scale_);
  const float off = r + static_cast<float>(1.6 * scale_);

  const gp::GraphicsState st = g.Save();
  g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
  gp::SolidBrush br(Col(c, 215.0f, Clamp01(a / 255.0f)));
  g.FillEllipse(&br, cx - off - r, cy - r, r * 2.0f, r * 2.0f);
  g.FillEllipse(&br, cx + off - r, cy - r, r * 2.0f, r * 2.0f);
  g.Restore(st);
}

} // namespace island
