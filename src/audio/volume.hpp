#pragma once
// System (master) volume via Core Audio, used by the scroll-wheel-on-capsule feature.
// UI thread only. Plain interface: no COM types leak out of the .cpp.

#include <memory>

namespace island {

class SystemVolume {
public:
  SystemVolume();
  ~SystemVolume();
  SystemVolume(const SystemVolume&) = delete;
  SystemVolume& operator=(const SystemVolume&) = delete;

  // Changes the default playback device's volume by `delta` (0.02 = 2 %), unmuting when raising.
  // On success returns true and reports the new level (0..1) and mute state.
  bool Nudge(float delta, float* level, bool* muted);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace island
