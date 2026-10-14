#pragma once
// Plain data describing "what is playing". No WinRT / GDI types in here, so any
// translation unit (UI, theme, settings) can include it cheaply.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace island {

// Square, premultiplied BGRA pixels (up to 128 px; the UI downsamples to the on-screen size).
struct ArtImage {
  int w = 0;
  int h = 0;
  std::vector<uint8_t> bgra;   // size == w * h * 4

  // Album accent, computed ONCE on the media worker thread right after the decode (ExtractAccent), so the
  // UI thread never samples pixels. Plain floats here: theme/accent.hpp includes this header, not vice versa.
  bool accent_ready = false;
  float accent_r = 150.0f, accent_g = 150.0f, accent_b = 160.0f;   // 0..255
  float accent_strength = 0.0f;                                    // 0..1
};

struct MediaInfo {
  bool has_session = false;       // false => nothing is playing / no player
  std::wstring title;
  std::wstring artist;
  std::wstring album;
  std::wstring source_app;        // e.g. "Spotify.exe", "chrome.exe"

  bool is_playing = false;
  bool can_play_pause = false;
  bool can_next = false;
  bool can_prev = false;

  std::shared_ptr<const ArtImage> art;   // null when the player gives no thumbnail

  // Playback timeline. Only read while lyrics mode is on (MediaSession::SetTimelineEnabled); it is used
  // INTERNALLY to pick the current lyric line and is never drawn (no progress bar).
  bool has_timeline = false;
  int64_t duration_ms = 0;        // 0 = unknown
  int64_t position_ms = 0;        // playback position at `position_stamp`
  std::chrono::steady_clock::time_point position_stamp{};
  double playback_rate = 1.0;

  // Position right now, extrapolated while playing; -1 when the player reports no timeline.
  int64_t PositionNowMs() const {
    if (!has_timeline) return -1;
    int64_t p = position_ms;
    if (is_playing) {
      const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - position_stamp).count();
      if (dt > 0) p += static_cast<int64_t>(static_cast<double>(dt) * playback_rate);
    }
    if (duration_ms > 0 && p > duration_ms) p = duration_ms;
    return p < 0 ? 0 : p;
  }
};

// Identity of a track for lyrics (deliberately without the duration, which some players report late).
inline std::wstring TrackId(const MediaInfo& m) {
  return m.title + L"\x1f" + m.artist + L"\x1f" + m.album;
}

} // namespace island