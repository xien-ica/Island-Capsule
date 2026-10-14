#pragma once
// Phase 2 – side-expand transport strip: [prev] [play/pause] [next].
// Pure drawing + hit-testing + hover/press animation; the window owner feeds it
// mouse state and receives clicks.

#include "theme/brushes.hpp"
#include "ui/gdip.hpp"

namespace island {

struct TransportState {
  bool playing = false;
  bool can_prev = false;
  bool can_play_pause = false;
  bool can_next = false;
};

class TransportStrip {
public:
  static constexpr int kPrev = 0;
  static constexpr int kPlayPause = 1;
  static constexpr int kNext = 2;

  // `inner_left`: the pill sits to the strip's left (strip expands to the right), so the part of
  // the strip that hides under the pill is on its left side.
  void Layout(double scale, int height_px, bool inner_left);
  int Width() const { return width_; }
  int Height() const { return height_; }
  int Overlap() const { return static_cast<int>(overlap_ + 0.5f); }     // hidden under the pill
  float SurfaceInset() const { return inset_; }                         // strip is shorter than the pill
  float SurfaceHeight() const { return surf_h_; }

  // Coordinates are relative to the strip's top-left. Returns -1 for none.
  int HitTest(int x, int y) const;
  void SetHot(int button) { hot_ = button; }
  void SetPressed(int button) { pressed_ = button; }
  void Reset() { hot_ = pressed_ = -1; }

  // Returns true while hover/press animations are still moving.
  bool Update(double dt);

  void Draw(Gdiplus::Graphics& g, float origin_x, float alpha, const GlassStyle& style,
            const TransportState& st) const;

  // ---- upward panel (lyrics mode) ----------------------------------------------------------------
  // The same three buttons on a panel that rises out of the TOP edge of the main capsule. The panel is as
  // wide as the capsule (left / right edges aligned); the caller clips it at the capsule's top edge.
  void LayoutUp(double scale);
  int UpHeight() const { return up_v_; }
  int UpWidth() const { return up_w_; }    // the tab's own width (independent of the capsule)   // height of the part that shows above the capsule's top edge
  // Coordinates relative to the panel's top-left (width = the capsule's width). -1 for none.
  int HitTestUp(int x, int y, int width) const;
  // Buttons only: the tab's body is part of the capsule's outline (drawn by the pill). (x, y) = tab top-left.
  void DrawUpButtons(Gdiplus::Graphics& g, float x, float y, float width, float alpha, const GlassStyle& style,
                     const TransportState& st) const;

private:
  void ButtonRect(int i, float* x, float* y, float* w, float* h) const;
  void DrawButton(Gdiplus::Graphics& g, int i, float cx, float cy, float diameter, float alpha,
                  const GlassStyle& style, const TransportState& st) const;

  double scale_ = 1.0;
  int width_ = 0;
  int height_ = 0;
  float pad_ = 0.0f;
  float pad_l_ = 0.0f;   // space before the first button (strip coordinates)
  float pad_r_ = 0.0f;   // space after the last button
  float btn_ = 0.0f;
  float spacing_ = 0.0f;
  float inset_ = 0.0f;
  float surf_h_ = 0.0f;
  float overlap_ = 0.0f;
  bool inner_left_ = true;

  int up_w_ = 0;           // upward tab: own width
  int up_v_ = 0;           // upward panel: visible height
  float up_pitch_ = 0.0f;  // distance between button centers
  float up_btn_ = 0.0f;    // button diameter

  int hot_ = -1;
  int pressed_ = -1;
  float hover_t_[3] = { 0, 0, 0 };
  float press_t_[3] = { 0, 0, 0 };
};

} // namespace island
