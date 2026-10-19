#pragma once
// Real audio source for the in-pill visualizer: WASAPI loopback (what the default playback device is playing, or
// just one app's audio via process loopback) -> 2048-point FFT -> kBands log-spaced levels in 0..1.
//
// The capture runs on its own thread, and only while the UI asks for it (SetActive(true)): nothing is opened
// while the capsule is paused / hidden / the visualizer is off. Nothing is recorded or stored: samples go
// through a small ring buffer and are discarded. Plain interface, no COM types leak out of the .cpp.

#include <memory>
#include <string>

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

  // UI thread. Which app the bands should follow: the media session's app id (MediaInfo::source_app), so only
  // that app's sound moves the visualizer. Empty = everything the default playback device plays. Cheap when the
  // value did not change. Per-app capture needs Windows 10 build 20348+ / Windows 11; on older systems the
  // system mix is used instead.
  void SetTargetApp(const std::wstring& app);

  // UI thread. True once per change of what the capture is doing ("capturing only process 1234 ...", or why a
  // per-app capture failed), with a short line for the app's log.
  bool TakeDiagnostic(std::wstring* line);

  State GetState() const;
  // Latest levels, 0..1 (any thread). Zeros while idle.
  void GetBands(float (&out)[kBands]) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace island
