#pragma once
// Marquee timeline for one line of text: pause -> scroll one full loop -> pause.
// The caller draws the text twice, `Loop()` pixels apart, shifted by `Offset()`,
// so the wrap-around is seamless.

namespace island {

class Marquee {
public:
  void Reset() {
    phase_ = Phase::Pause;
    t_ = 0.0;
    off_ = 0.0f;
    loop_ = 0.0f;
  }

  // Returns true while the line needs animation frames.
  bool Update(double dt, bool overflow, float text_w, double scale) {
    loop_ = text_w + static_cast<float>(kGap * scale);
    if (!overflow) {
      off_ = 0.0f;
      phase_ = Phase::Pause;
      t_ = 0.0;
      return false;
    }
    if (phase_ == Phase::Pause) {
      t_ += dt;
      if (t_ >= kPauseSec) phase_ = Phase::Scroll;
    } else {
      off_ += static_cast<float>(kSpeed * scale * dt);
      if (off_ >= loop_) {
        off_ = 0.0f;
        phase_ = Phase::Pause;
        t_ = 0.0;
      }
    }
    return true;
  }

  float Offset() const { return off_; }
  float Loop() const { return loop_; }

private:
  enum class Phase { Pause, Scroll };
  static constexpr double kPauseSec = 1.6;
  static constexpr double kSpeed = 34.0;   // logical px / second
  static constexpr double kGap = 32.0;     // logical px between the two copies

  Phase phase_ = Phase::Pause;
  double t_ = 0.0;
  float off_ = 0.0f;
  float loop_ = 0.0f;
};

} // namespace island
