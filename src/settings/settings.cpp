// Phase 5 – settings.ini load / default-file creation / change polling.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <string>

#include "settings/settings.hpp"

namespace island {

namespace {

std::wstring Lower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return s;
}

std::wstring Trim(const std::wstring& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::iswspace(s[a])) ++a;
  while (b > a && std::iswspace(s[b - 1])) --b;
  return s.substr(a, b - a);
}

std::wstring ReadString(const wchar_t* path, const wchar_t* key, const wchar_t* def) {
  wchar_t buf[512];
  GetPrivateProfileStringW(L"capsule", key, def, buf, 512, path);
  return Trim(buf);
}

bool ReadBool(const wchar_t* path, const wchar_t* key, bool def) {
  const std::wstring v = Lower(ReadString(path, key, def ? L"true" : L"false"));
  if (v == L"1" || v == L"true" || v == L"yes" || v == L"on") return true;
  if (v == L"0" || v == L"false" || v == L"no" || v == L"off") return false;
  return def;
}

std::vector<std::wstring> SplitList(const std::wstring& s) {
  std::vector<std::wstring> out;
  std::wstring cur;
  for (wchar_t c : s) {
    // comma, semicolon, slash, pipe or whitespace, e.g. "Spotify.exe, chrome.exe"
    if (c == L',' || c == L';' || c == L'/' || c == L'|' || std::iswspace(c)) {
      cur = Trim(cur);
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  cur = Trim(cur);
  if (!cur.empty()) out.push_back(cur);
  return out;
}

} // namespace

const wchar_t* AnchorName(PillAnchor a) {
  switch (a) {
    case PillAnchor::Left:  return L"left";
    case PillAnchor::Right: return L"right";
    default:                return L"center";
  }
}

const wchar_t* VizName(VizStyle v) {
  switch (v) {
    case VizStyle::Off:   return L"off";
    case VizStyle::Wave:  return L"wave";
    case VizStyle::Glow:  return L"glow";
    default:              return L"ticks";
  }
}

bool ParseAnchor(const std::wstring& s, PillAnchor* out) {
  const std::wstring v = Lower(Trim(s));
  if (v == L"left")   { *out = PillAnchor::Left;   return true; }
  if (v == L"center") { *out = PillAnchor::Center; return true; }
  if (v == L"right")  { *out = PillAnchor::Right;  return true; }
  return false;
}

bool ParseViz(const std::wstring& s, VizStyle* out) {
  const std::wstring v = Lower(Trim(s));
  if (v == L"off" || v == L"none") { *out = VizStyle::Off;   return true; }
  if (v == L"ticks" || v == L"v1") { *out = VizStyle::Ticks; return true; }
  if (v == L"wave" || v == L"v2")  { *out = VizStyle::Wave;  return true; }
  if (v == L"glow" || v == L"v4")  { *out = VizStyle::Glow;  return true; }
  return false;
}

std::wstring ResolveFontFamily(const std::wstring& font) {
  const std::wstring v = Lower(Trim(font));
  if (v.empty() || v == L"default") return L"Segoe UI Variable Text";
  return Trim(font);   // preserve the user's casing for the family name
}

std::wstring SettingsPath() {
  wchar_t base[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return L"settings.ini";   // fall back to the working dir
  return std::wstring(base) + L"\\IslandCapsule\\settings.ini";
}

Settings LoadSettings() {
  Settings s;
  const std::wstring path = SettingsPath();
  const wchar_t* p = path.c_str();
  if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return s;

  PillAnchor a;
  if (ParseAnchor(ReadString(p, L"position", L"center"), &a)) s.anchor = a;
  s.margin = (std::clamp)(static_cast<int>(GetPrivateProfileIntW(L"capsule", L"margin", s.margin, p)), 0, 200);
  s.max_width = (std::clamp)(static_cast<int>(GetPrivateProfileIntW(L"capsule", L"max_width", s.max_width, p)), 200, 600);

  VizStyle v;
  if (ParseViz(ReadString(p, L"visualizer", L"ticks"), &v)) s.visualizer = v;
  s.visualizer_audio = ReadBool(p, L"visualizer_audio", s.visualizer_audio);

  s.hide_fullscreen = ReadBool(p, L"hide_fullscreen", s.hide_fullscreen);
  s.scroll_volume = ReadBool(p, L"scroll_volume", s.scroll_volume);
  s.scroll_step = (std::clamp)(static_cast<int>(GetPrivateProfileIntW(L"capsule", L"scroll_step", s.scroll_step, p)), 1, 10);
  s.shift_scroll_skip = ReadBool(p, L"shift_scroll_skip", s.shift_scroll_skip);
  s.click_actions = ReadBool(p, L"click_actions", s.click_actions);
  s.strip_left = Lower(ReadString(p, L"strip_side", L"right")) == L"left";
  s.collapse_paused = ReadBool(p, L"collapse_paused", s.collapse_paused);
  s.seam_glow = ReadBool(p, L"seam_glow", s.seam_glow);
  s.overshoot = ReadBool(p, L"overshoot", s.overshoot);
  s.hover_scale = ReadBool(p, L"hover_scale", s.hover_scale);
  s.text_slide = ReadBool(p, L"text_slide", s.text_slide);
  s.reduced_motion = ReadBool(p, L"reduced_motion", s.reduced_motion);
  s.preferred_apps = SplitList(ReadString(p, L"preferred_apps", L""));
  s.selected_only = Lower(ReadString(p, L"media_source", L"all")) == L"selected";
  s.selected_apps = SplitList(ReadString(p, L"selected_apps", L""));
  s.lyrics_enabled = ReadBool(p, L"lyrics_enabled", s.lyrics_enabled);
  s.lyrics_mode = ReadBool(p, L"lyrics_mode", s.lyrics_mode);
  s.lyrics_cache = ReadBool(p, L"lyrics_cache", s.lyrics_cache);
  // GetPrivateProfileInt cannot be trusted with negative numbers, so parse the text.
  s.lyrics_offset_ms = (std::clamp)(static_cast<int>(wcstol(ReadString(p, L"lyrics_offset_ms", L"0").c_str(), nullptr, 10)), -5000, 5000);
  {
    const std::wstring f = ReadString(p, L"font", L"default");
    s.font = f.empty() ? L"default" : f;
  }
  return s;
}

bool EnsureSettingsFile() {
  const std::wstring path = SettingsPath();
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;

  const size_t slash = path.find_last_of(L'\\');
  if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);

  static const char kDefault[] =
      "; Island Capsule settings - saved changes apply live.\r\n"
      "[capsule]\r\n"
      "; left | center | right\r\n"
      "position=center\r\n"
      "; logical px from the taskbar edge (or the tray, when right)\r\n"
      "margin=8\r\n"
      "; widest the pill may grow, logical px (200..600)\r\n"
      "max_width=320\r\n"
      "; fade the capsule out while a fullscreen app, game or video is running\r\n"
      "hide_fullscreen=true\r\n"
      "; mouse wheel over the capsule changes the system volume (percent per notch, 1..10)\r\n"
      "scroll_volume=true\r\n"
      "scroll_step=2\r\n"
      "; Shift + wheel over the capsule skips tracks (wheel down = next, up = previous)\r\n"
      "shift_scroll_skip=true\r\n"
      "; middle-click = play / pause, right-click = menu (copy title, lyrics, settings, quit)\r\n"
      "click_actions=true\r\n"
      "; ticks | wave | glow | off\r\n"
      "visualizer=ticks\r\n"
      "; true: the visualizer follows the real audio (captures what the default playback device plays, nothing is\r\n"
      "; recorded or stored); false: synthetic motion, no audio capture\r\n"
      "visualizer_audio=true\r\n"
      "; which side the transport strip opens on: left | right\r\n"
      "strip_side=right\r\n"
      "; paused => the capsule collapses to just the visualizer (hover reopens it)\r\n"
      "collapse_paused=true\r\n"
      "; accent glow that crawls out when the transport strip expands\r\n"
      "seam_glow=true\r\n"
      "; springy overshoot when the transport strip / controls open\r\n"
      "overshoot=true\r\n"
      "; the capsule grows slightly on hover\r\n"
      "hover_scale=true\r\n"
      "; text slides when the track or lyric line changes\r\n"
      "text_slide=true\r\n"
      "; master switch: true turns overshoot, hover scale, text slide and seam glow all off\r\n"
      "reduced_motion=false\r\n"
      "; comma separated app-id fragments, e.g. spotify,chrome (restart to apply)\r\n"
      "preferred_apps=\r\n"
      "; which apps the capsule listens to: all | selected\r\n"
      "; selected = ONLY the apps in selected_apps (no media from anything else; capsule hides)\r\n"
      "media_source=all\r\n"
      "; executable names separated by comma, semicolon, slash, pipe or space, e.g. Spotify.exe, chrome.exe\r\n"
      "selected_apps=\r\n"
      "; synced lyrics from lrclib.net (Ctrl+L over the capsule toggles lyrics mode)\r\n"
      "lyrics_enabled=true\r\n"
      "; saved by Ctrl+L: the capsule shows the current lyric line instead of title / artist\r\n"
      "lyrics_mode=false\r\n"
      "; shifts lyric timing for every song, in ms, -5000..5000 (positive = lyrics appear later).\r\n"
      "; One song only: Ctrl+Left / Ctrl+Right over the capsule nudges it by 100 ms (Ctrl+Down resets it).\r\n"
      "lyrics_offset_ms=0\r\n"
      "; keep fetched lyrics and per-song nudges on disk (%APPDATA%\\IslandCapsule\\lyrics); false = no disk cache\r\n"
      "lyrics_cache=true\r\n"
      "; capsule text font: default (Segoe UI Variable Text) | Segoe UI | Bahnschrift | Calibri | Georgia | Consolas\r\n"
      "; or any installed family name (falls back if missing)\r\n"
      "font=default\r\n";

  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = WriteFile(f, kDefault, static_cast<DWORD>(sizeof(kDefault) - 1), &written, nullptr);
  CloseHandle(f);
  return ok != FALSE;
}

bool SaveSettingValue(const wchar_t* key, const std::wstring& value) {
  if (!EnsureSettingsFile()) return false;
  const std::wstring path = SettingsPath();
  return WritePrivateProfileStringW(L"capsule", key, value.c_str(), path.c_str()) != FALSE;
}

bool SaveSettingBool(const wchar_t* key, bool value) {
  return SaveSettingValue(key, value ? L"true" : L"false");
}

bool SaveSettingInt(const wchar_t* key, int value) {
  return SaveSettingValue(key, std::to_wstring(value));
}

std::wstring JoinList(const std::vector<std::wstring>& items) {
  std::wstring out;
  for (const auto& it : items) {
    if (!out.empty()) out += L", ";
    out += it;
  }
  return out;
}

std::vector<std::wstring> ParseList(const std::wstring& text) { return SplitList(text); }

bool SettingsWatcher::Poll(Settings* out) {
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  const std::wstring path = SettingsPath();
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return false;
  const unsigned long long stamp =
      (static_cast<unsigned long long>(fa.ftLastWriteTime.dwHighDateTime) << 32) |
      fa.ftLastWriteTime.dwLowDateTime;
  if (stamp == stamp_) return false;
  stamp_ = stamp;
  if (out) *out = LoadSettings();
  return true;
}

} // namespace island
