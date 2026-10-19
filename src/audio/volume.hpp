#pragma once
// System (master) volume and per-app volume via Core Audio, used by the scroll-wheel-on-capsule feature.
// UI thread only. Plain interface: no COM types leak out of the .cpp.

#include <memory>
#include <string>

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

  // Same, but only for the app that owns the current media session (its entry in Windows' Volume Mixer),
  // leaving the system volume alone. `source_app` is MediaInfo::source_app (an AppUserModelId such as
  // "Spotify.exe", "Chrome" or "SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify"); it is matched against the
  // executable names of the processes that currently have an audio session. Every matching session (a browser
  // has several) is set to the same new level. Returns false when no matching session exists, e.g. the app
  // has not played any sound yet.
  bool NudgeApp(const std::wstring& source_app, float delta, float* level, bool* muted);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace island
