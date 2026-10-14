// Phase 2 – transport strip drawing (vector glyphs, no image assets).
// The strip is its own (slightly shorter) capsule that tucks under the main pill.

#include "ui/transport.hpp"

#include <cmath>

namespace gp = Gdiplus;

namespace island {

namespace {

bool Approach(float& v, float target, double dt, double rate) {
  const float d = target - v;
  if (std::fabs(d) < 0.01f) {
    const bool changed = v != target;
    v = target;
    return changed;
  }
  v += d * static_cast<float>(1.0 - std::exp(-rate * dt));
  return true;
}

void DrawGlyph(gp::Graphics& g, gp::Brush& ink, int which, bool playing,
               float cx, float cy, float gl, double scale) {
  const float bar_w = static_cast<float>(2.4 * scale);
  switch (which) {
    case TransportStrip::kPrev: {
      gp::PointF tri[3] = { { cx + gl * 0.6f, cy - gl * 0.9f },
                            { cx + gl * 0.6f, cy + gl * 0.9f },
                            { cx - gl * 0.9f, cy } };
      g.FillPolygon(&ink, tri, 3);
      g.FillRectangle(&ink, cx - gl * 0.9f - bar_w * 0.4f - bar_w, cy - gl * 0.9f, bar_w, gl * 1.8f);
      break;
    }
    case TransportStrip::kNext: {
      gp::PointF tri[3] = { { cx - gl * 0.6f, cy - gl * 0.9f },
                            { cx - gl * 0.6f, cy + gl * 0.9f },
                            { cx + gl * 0.9f, cy } };
      g.FillPolygon(&ink, tri, 3);
      g.FillRectangle(&ink, cx + gl * 0.9f + bar_w * 0.4f, cy - gl * 0.9f, bar_w, gl * 1.8f);
      break;
    }
    case TransportStrip::kPlayPause:
    default: {
      if (playing) {
        const float w = static_cast<float>(3.4 * scale);
        const float off = static_cast<float>(2.0 * scale);
        g.FillRectangle(&ink, cx - off - w, cy - gl, w, gl * 2.0f);
        g.FillRectangle(&ink, cx + off, cy - gl, w, gl * 2.0f);
      } else {
        gp::PointF tri[3] = { { cx - gl * 0.7f, cy - gl },
                              { cx - gl * 0.7f, cy + gl },
                              { cx + gl * 0.95f, cy } };
        g.FillPolygon(&ink, tri, 3);
      }
      break;
    }
  }
}

} // namespace

void TransportStrip::Layout(double scale, int height_px, bool inner_left) {
  scale_ = scale;
  height_ = height_px;
  inner_left_ = inner_left;
  inset_ = std::round(static_cast<float>(1.0 * scale));                 // 1 px shorter than the pill, top and bottom
  surf_h_ = static_cast<float>(height_px) - 2.0f * inset_;
  pad_ = std::round(static_cast<float>(5.0 * scale));                   // roomier: edge padding
  btn_ = surf_h_ - 2.0f * pad_;
  spacing_ = std::round(static_cast<float>(5.0 * scale));               // roomier: gap between buttons
  overlap_ = static_cast<float>(height_px) * 0.6f;                      // tucked under the pill

  const float inner_pad = std::round(static_cast<float>(5.0 * scale));    // gap next to the main capsule
  const float outer_pad = std::round(static_cast<float>(12.0 * scale));   // far end of the strip
  pad_l_ = inner_left ? overlap_ + inner_pad : outer_pad;
  pad_r_ = inner_left ? outer_pad : overlap_ + inner_pad;
  width_ = static_cast<int>(std::lround(pad_l_ + 3.0f * btn_ + 2.0f * spacing_ + pad_r_));
}

void TransportStrip::ButtonRect(int i, float* x, float* y, float* w, float* h) const {
  *x = pad_l_ + static_cast<float>(i) * (btn_ + spacing_);
  *y = inset_ + pad_;
  *w = btn_;
  *h = btn_;
}

int TransportStrip::HitTest(int x, int y) const {
  for (int i = 0; i < 3; ++i) {
    float bx, by, bw, bh;
    ButtonRect(i, &bx, &by, &bw, &bh);
    const float half = spacing_ * 0.5f;
    if (x >= bx - half && x < bx + bw + half && y >= 0 && y < height_) return i;
  }
  return -1;
}

bool TransportStrip::Update(double dt) {
  bool moving = false;
  for (int i = 0; i < 3; ++i) {
    const float hover_target = hot_ == i ? 1.0f : 0.0f;
    const float press_target = (pressed_ == i && hot_ == i) ? 1.0f : 0.0f;
    moving |= Approach(hover_t_[i], hover_target, dt, 18.0);
    moving |= Approach(press_t_[i], press_target, dt, 30.0);
  }
  return moving;
}

void TransportStrip::Draw(gp::Graphics& g, float origin_x, float alpha, const GlassStyle& style,
                          const TransportState& st) const {
  if (alpha <= 0.003f || width_ <= 0) return;

  const gp::GraphicsState saved = g.Save();
  g.TranslateTransform(origin_x, 0.0f);

  // Strip capsule: same glass language, a bit shorter than the pill. The pill is drawn on top of
  // its inner end, so the two read as separate capsules that overlap.
  {
    const float W = static_cast<float>(width_);
    gp::GraphicsPath body;
    AddPill(body, 0.0f, inset_, W, surf_h_);
    gp::SolidBrush fill(Col(style.fill2, style.fill2_a, alpha));
    g.FillPath(&fill, &body);

    gp::GraphicsPath rim_path;
    AddPill(rim_path, 0.5f, inset_ + 0.5f, W - 1.0f, surf_h_ - 1.0f);
    gp::Pen rim(Col(style.rim, style.rim_a, alpha), 1.0f);
    g.DrawPath(&rim, &rim_path);
  }

  for (int i = 0; i < 3; ++i) {
    float bx, by, bw, bh;
    ButtonRect(i, &bx, &by, &bw, &bh);
    DrawButton(g, i, bx + bw * 0.5f, by + bh * 0.5f, bw, alpha, style, st);
  }

  g.Restore(saved);
}

// One button (hover / press disc + glyph) centered at (cx, cy).
void TransportStrip::DrawButton(gp::Graphics& g, int i, float cx, float cy, float diameter, float alpha,
                                const GlassStyle& style, const TransportState& st) const {
  const bool enabled[3] = { st.can_prev, st.can_play_pause, st.can_next };
  const float press = press_t_[i];
  const float hover = hover_t_[i];

  if (enabled[i]) {
    const float r = diameter * 0.5f * (1.0f - 0.14f * press);
    const int bg_a = static_cast<int>(hover * 30.0f + press * 38.0f);
    if (bg_a > 0) {
      gp::SolidBrush bg(Col(style.ink, static_cast<float>(bg_a), alpha));
      g.FillEllipse(&bg, cx - r, cy - r, r * 2.0f, r * 2.0f);
    }
  }

  const float gl = static_cast<float>(5.5 * scale_) * (1.0f - 0.10f * press);
  gp::SolidBrush glyph(Col(style.ink, enabled[i] ? 235.0f : 85.0f, alpha));
  DrawGlyph(g, glyph, i, st.playing, cx, cy, gl, scale_);
}

// ---- upward panel (lyrics mode) -------------------------------------------------------------------

void TransportStrip::LayoutUp(double scale) {
  scale_ = scale;
  up_v_ = static_cast<int>(std::lround(34.0 * scale));
  up_btn_ = static_cast<float>(up_v_) - 2.0f * std::round(static_cast<float>(5.0 * scale));
  up_pitch_ = std::round(static_cast<float>(38.0 * scale));
  up_w_ = static_cast<int>(std::lround(2.0f * up_pitch_ + up_btn_ + 2.0f * 10.0f * static_cast<float>(scale)));
}

int TransportStrip::HitTestUp(int x, int y, int width) const {
  if (y < 0 || y >= up_v_) return -1;
  const float mid = static_cast<float>(width) * 0.5f;
  for (int i = 0; i < 3; ++i) {
    const float cx = mid + static_cast<float>(i - 1) * up_pitch_;
    if (std::fabs(static_cast<float>(x) - cx) < up_pitch_ * 0.5f) return i;
  }
  return -1;
}

void TransportStrip::DrawUpButtons(gp::Graphics& g, float x, float y, float width, float alpha,
                                   const GlassStyle& style, const TransportState& st) const {
  if (alpha <= 0.003f || width <= 4.0f || up_v_ <= 0) return;
  const gp::GraphicsState saved = g.Save();
  g.TranslateTransform(x, y);
  const float V = static_cast<float>(up_v_);
  const float mid = width * 0.5f;
  for (int i = 0; i < 3; ++i)
    DrawButton(g, i, mid + static_cast<float>(i - 1) * up_pitch_, V * 0.5f, up_btn_, alpha, style, st);
  g.Restore(saved);
}

} // namespace island
