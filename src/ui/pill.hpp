#pragma once
// Layered pill window drawn with GDI+ (per-pixel alpha).
// Phase 2: eased autofit width, marquee, side-expand transport strip.
// Phase 3: album color.  Phase 4: in-pill visualizer.
// Phase 5: settings, spring motion, art crossfade, skip-direction text slide,
//          idle hide, throttled frame pacing.
// Polish: one shared capsule when the strip expands, no scaling (pixel-aligned drawing),
//         sharper cover + text.

#include <windows.h>
#include <functional>
#include <string>
#include <memory>

#include "lyrics/lrc.hpp"
#include "media/media_info.hpp"
#include "settings/settings.hpp"

namespace island {

enum class PillCommand { Previous, PlayPause, Next };

class PillWindow {
public:
  PillWindow();
  ~PillWindow();
  PillWindow(const PillWindow&) = delete;
  PillWindow& operator=(const PillWindow&) = delete;

  // Applies (or re-applies, live) all user settings.
  void ApplySettings(const Settings& settings);
  void SetAnchor(PillAnchor anchor, int margin_logical_px);

  // Called on the UI thread when a transport button is clicked.
  void SetCommandHandler(std::function<void(PillCommand)> fn);
  // Diagnostics (fullscreen hide, volume wheel): lines include their own "\r\n".
  void SetLogHandler(std::function<void(const std::wstring&)> fn);

  // Tell the pill a skip was requested from elsewhere (keyboard, hotkey) so the next
  // track change slides in the matching direction. -1 = previous, +1 = next.
  void NotifySkip(int dir);

  bool Create();
  void Destroy();

  // UI thread only.
  void SetMedia(std::shared_ptr<const MediaInfo> info);

  // Lyrics mode (UI thread). The loader owns the fetcher and feeds the latest snapshot here; the pill
  // matches it against the current track itself, so a stale result can never show for another song.
  void SetLyrics(std::shared_ptr<const LyricsSnapshot> lyrics);
  // True while the user wants lyrics (lyrics_enabled && Ctrl+L state): the loader then reads the
  // playback timeline and runs lookups. False => no lookups, no timeline wakeups.
  bool LyricsWanted() const;
  // Per-song timing nudge (Ctrl+Left / Ctrl+Right over the capsule while lyrics show, Ctrl+Down resets).
  // Called on the UI thread with (track id, new per-song nudge in ms); the loader forwards it to the fetcher,
  // which saves it in the cache entry. The effective offset is lyrics_offset_ms + this nudge.
  void SetLyricsOffsetHandler(std::function<void(const std::wstring&, int)> fn);

  // Call every loop iteration from the UI thread.
  void Tick();

  // How long the caller may sleep before the next Tick(): ~16 ms during motion,
  // ~33 ms while only the visualizer runs, 250 ms when idle.
  unsigned NextWaitMs() const;

  // True while any animation is running.
  bool NeedsFrames() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace island
