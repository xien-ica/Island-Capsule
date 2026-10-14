#pragma once
// Phase 5 – tiny motion helpers shared by the pill and the visualizer.

#include <algorithm>
#include <cmath>

namespace island {

// Damped spring toward a target. Sub-stepped so big frame gaps stay stable.
struct Spring {
  double x = 0.0;
  double v = 0.0;

  void Snap(double value) { x = value; v = 0.0; }

  // Returns true if `x` changed during this call (=> a redraw is needed).
  bool Step(double dt, double target, double k, double c) {
    const double before = x;
    const int n = (std::max)(1, static_cast<int>(std::ceil(dt / (1.0 / 240.0))));
    const double h = dt / n;
    for (int i = 0; i < n; ++i) {
      v += (k * (target - x) - c * v) * h;
      x += v * h;
    }
    if (std::fabs(target - x) < 0.0015 && std::fabs(v) < 0.02) {
      x = target;
      v = 0.0;
    }
    return x != before;
  }
};

// Damping that gives no overshoot for stiffness k.
inline double CriticalDamping(double k) { return 2.0 * std::sqrt(k); }

inline double EaseOutCubic(double t) {
  t = (std::clamp)(t, 0.0, 1.0);
  const double u = 1.0 - t;
  return 1.0 - u * u * u;
}

} // namespace island
