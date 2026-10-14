#pragma once
// Real audio source for the in-pill visualizer: WASAPI loopback (what the default playback device is playing)
// -> 2048-point FFT -> kBands log-spaced levels in 0..1.
//
// The capture runs on its own thread, and only while the UI asks for it (SetActive(true)): nothing is opened
// while the capsule is paused / hidden / the visualizer is off. Nothing is recorded or stored: samples go
// through a small ring buffer and are discarded. Plain interface, no COM types leak out of the .cpp.

#include <memory>

namespace island {

class AudioSpectrum {
public:
  static constexpr int kBands = 6;   // must match Visualizer::kBands

  enum class State {
    Idle,          // not capturing (inactive)
    Live,          // GetBands() holds real levels
    Unavailable,   // no device / unsupported format: the caller should fall back to synthetic motion
  };

  AudioSpectrum();
  ~AudioSpectrum();
  AudioSpectrum(const AudioSpectrum&) = delete;
  AudioSpectrum& operator=(const AudioSpectrum&) = delete;

  // UI thread. Cheap when the value did not change; the worker thread is created on the first SetActive(true).
  void SetActive(bool on);

  State GetState() const;
  // Latest levels, 0..1 (any thread). Zeros while idle.
  void GetBands(float (&out)[kBands]) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace island
