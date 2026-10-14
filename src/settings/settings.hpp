#pragma once
// Phase 5 – user settings. Plain data only (no WinRT / GDI+ types).
//
// File: %APPDATA%\IslandCapsule\settings.ini  (created with defaults on first run,
// hot-reloaded while the loader is running).

#include <string>
#include <vector>

namespace island {

enum class PillAnchor { Left, Center, Right };
enum class VizStyle { Off, Ticks, Wave, Glow };

struct Settings {
  PillAnchor anchor = PillAnchor::Center;
  int margin = 8;                    // logical px from the taskbar edge / tray
  int max_width = 320;               // logical px, clamped to 200..600
  VizStyle visualizer = VizStyle::Ticks;
  bool visualizer_audio = true;      // visualizer follows the real audio (WASAPI loopback); false => synthetic motion
  bool hide_fullscreen = true;       // fade the capsule out while a fullscreen app / game / video is up
  bool scroll_volume = true;         // mouse wheel over the capsule changes the system volume
  int scroll_step = 2;               // percent per wheel notch (1..10)
  bool shift_scroll_skip = true;     // Shift + wheel over the capsule skips tracks (down = next, up = previous)
  bool click_actions = true;         // middle-click = play / pause, right-click = context menu
  bool strip_left = false;           // transport strip opens on the left of the main capsule (default: right)
  bool collapse_paused = true;       // paused => capsule collapses to just the visualizer
  bool seam_glow = true;             // accent glow crawling out when the strip expands
  bool overshoot = true;             // springy overshoot when the strip / tab opens
  bool hover_scale = true;           // capsule grows slightly on hover
  bool text_slide = true;            // text slides when the track / lyric line changes
  bool reduced_motion = false;       // legacy master switch (--reduced): turns all of the above + seam glow off
  std::vector<std::wstring> preferred_apps;   // e.g. spotify, chrome (prefer these, fall back to Windows' pick)
  bool selected_only = false;                 // media_source=selected: ONLY the apps below are used
  std::vector<std::wstring> selected_apps;    // e.g. Spotify.exe, chrome.exe

  // Lyrics mode (src/lyrics, LRCLIB). Ctrl+L over the capsule toggles lyrics_mode.
  bool lyrics_enabled = true;        // master switch: false => Ctrl+L does nothing, no lookups, no timeline reads
  bool lyrics_mode = false;          // the user wants the capsule to show lyrics (saved when toggled, survives restarts)
  int lyrics_offset_ms = 0;          // shifts lyric timing for ALL songs: positive = lyrics appear later (-5000..5000)
  bool lyrics_cache = true;          // keep fetched lyrics (and per-song timing nudges) on disk; false => no disk cache
};

std::wstring SettingsPath();
Settings LoadSettings();                       // missing file / keys => defaults
bool EnsureSettingsFile();                     // writes a commented default file if missing

// Writes one key of the [capsule] section (keeps the file's comments and other keys).
// The running loader picks the change up through SettingsWatcher within ~1 s.
bool SaveSettingValue(const wchar_t* key, const std::wstring& value);
bool SaveSettingBool(const wchar_t* key, bool value);
bool SaveSettingInt(const wchar_t* key, int value);
std::wstring JoinList(const std::vector<std::wstring>& items);   // "a, b, c"
std::vector<std::wstring> ParseList(const std::wstring& text);   // same splitting as the ini

const wchar_t* AnchorName(PillAnchor a);
const wchar_t* VizName(VizStyle v);
bool ParseAnchor(const std::wstring& s, PillAnchor* out);
bool ParseViz(const std::wstring& s, VizStyle* out);

// Polls the file's modification time.
class SettingsWatcher {
public:
  // True when the file changed (or on the first call if it exists); `out` is reloaded.
  bool Poll(Settings* out);

private:
  unsigned long long stamp_ = 0;
};

} // namespace island
