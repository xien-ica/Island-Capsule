// Settings window - tabs (Capsule / Behavior / Media), toggles, sliders, segmented buttons.
// Everything is custom drawn with GDI+ into a double buffer; only the two app-list text boxes
// are real EDIT controls.

#include "ui/settings_window.hpp"

#include "ui/gdip.hpp"   // windows.h + gdiplus.h + AddPill / AddRoundRect

#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lyrics/lrclib.hpp"
#include "settings/settings.hpp"
#include "ui/tray.hpp"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

namespace island {

namespace {

namespace gp = Gdiplus;

constexpr wchar_t kClass[] = L"IslandCapsuleSettings";
constexpr int kClientW = 500;   // logical px
constexpr int kClientH = 540;
constexpr UINT_PTR kEditTimer = 1;
constexpr int kEditId = 2001;
constexpr int kTabCount = 3;
const wchar_t* const kTabNames[kTabCount] = { L"Capsule", L"Behavior", L"Media" };

// ---------------------------------------------------------------------------------------------
// Theme

struct Palette {
  bool dark = false;
  gp::Color bg, card, border, divider, text, sub, accent, white, off, segBg, segHover;
  COLORREF bgRef = 0, textRef = 0, fieldRef = 0;
};

gp::Color C(int r, int g, int b) { return gp::Color(255, static_cast<BYTE>(r), static_cast<BYTE>(g), static_cast<BYTE>(b)); }

bool SystemUsesDark() {
  DWORD v = 1, sz = sizeof(v);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
    return false;
  return v == 0;
}

Palette MakePalette(bool dark) {
  Palette p;
  p.dark = dark;
  p.white = C(255, 255, 255);
  if (dark) {
    p.bg = C(32, 32, 32);       p.card = C(43, 43, 43);     p.border = C(60, 60, 60);
    p.divider = C(56, 56, 56);  p.text = C(255, 255, 255);  p.sub = C(160, 160, 164);
    p.accent = C(104, 128, 255); p.off = C(104, 104, 108);  p.segBg = C(28, 28, 28);
    p.segHover = C(58, 58, 60);
    p.bgRef = RGB(32, 32, 32);  p.textRef = RGB(255, 255, 255);  p.fieldRef = RGB(30, 30, 30);
  } else {
    p.bg = C(243, 243, 243);    p.card = C(255, 255, 255);  p.border = C(226, 226, 228);
    p.divider = C(238, 238, 240); p.text = C(27, 27, 27);   p.sub = C(98, 98, 104);
    p.accent = C(80, 104, 240); p.off = C(176, 176, 182);   p.segBg = C(238, 238, 241);
    p.segHover = C(226, 226, 230);
    p.bgRef = RGB(243, 243, 243); p.textRef = RGB(27, 27, 27);   p.fieldRef = RGB(247, 247, 249);
  }
  return p;
}

std::wstring Trim(const std::wstring& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::iswspace(s[a])) ++a;
  while (b > a && std::iswspace(s[b - 1])) --b;
  return s.substr(a, b - a);
}

// ---------------------------------------------------------------------------------------------
// Model

enum class Kind { Toggle, Segmented, Slider, EditBox, Buttons };

struct Row {
  Kind kind = Kind::Toggle;
  std::wstring title, caption;
  std::vector<std::wstring> options;   // Segmented / Buttons
  float segw = 60.0f;                  // logical px per option (Segmented)
  int lo = 0, hi = 1, step = 1;        // Slider
  std::wstring suffix;                 // Slider value suffix
  int edit = -1;                       // EditBox: index into Ui::edits
  std::function<int()> get;
  std::function<void(int)> set;        // Buttons: called with the button index on click

  gp::RectF rect, ctrl;                // device px, filled by Layout()
  float Height() const { return kind == Kind::EditBox ? 84.0f : 46.0f; }
};

struct Section {
  std::wstring title;
  std::vector<Row> rows;
  gp::RectF head, card;
};

struct Hot {
  bool sb = false;   // pointer over the scrollbar thumb
  int tab = -1;
  int row = -1;
  int part = -1;
  bool link = false;
  bool operator==(const Hot& o) const { return sb == o.sb && tab == o.tab && row == o.row && part == o.part && link == o.link; }
  bool operator!=(const Hot& o) const { return !(*this == o); }
  bool Interactive() const { return sb || tab >= 0 || row >= 0 || link; }
};

struct Ui {
  HINSTANCE hinst = nullptr;
  HWND hwnd = nullptr;
  ULONG_PTR gdip_token = 0;
  bool gdip_ok = false;
  bool ready = false;

  Settings cfg;
  bool autostart = false;

  Palette pal;
  float s = 1.0f;   // DPI scale
  int page = 0;

  std::vector<Section> pages[kTabCount];
  std::vector<Row*> flat;   // rows of the current page, in order
  gp::RectF tab_rect[kTabCount];
  gp::RectF link_rect;

  HWND edits[2] = {};
  bool edits_dirty = false;
  bool loading = false;
  HFONT edit_font = nullptr;
  HBRUSH field_brush = nullptr;

  std::unique_ptr<gp::FontFamily> fam;
  std::unique_ptr<gp::Font> f_title, f_cap, f_head, f_tab, f_seg, f_link, f_link_u;

  Hot hot;
  int drag = -1;
  bool tracking = false;

  // vertical scrolling of the page content (between the tab bar and the footer)
  float scroll = 0.0f;        // px the content is shifted up
  float max_scroll = 0.0f;
  float view_top = 0.0f, view_bot = 0.0f;
  HICON icon_big = nullptr, icon_small = nullptr;   // the capsule (tray) icon, shown in the title bar
  bool sb_drag = false;
  float sb_grab = 0.0f;       // pointer offset inside the thumb while dragging

  ~Ui() {
    f_title.reset(); f_cap.reset(); f_head.reset(); f_tab.reset();
    f_seg.reset(); f_link.reset(); f_link_u.reset(); fam.reset();   // before GdiplusShutdown
    if (gdip_ok) gp::GdiplusShutdown(gdip_token);
    if (edit_font) DeleteObject(edit_font);
    if (field_brush) DeleteObject(field_brush);
    if (icon_big) DestroyIcon(icon_big);
    if (icon_small) DestroyIcon(icon_small);
  }

  // ---- model ---------------------------------------------------------------------------------

  Row MakeToggle(const wchar_t* title, const wchar_t* caption, std::function<int()> get, std::function<void(int)> set) {
    Row r;
    r.kind = Kind::Toggle; r.title = title; r.caption = caption;
    r.get = std::move(get); r.set = std::move(set);
    return r;
  }

  Row BoolRow(const wchar_t* title, const wchar_t* caption, bool Settings::*field, const wchar_t* key) {
    return MakeToggle(title, caption,
                      [this, field] { return cfg.*field ? 1 : 0; },
                      [this, field, key](int v) { cfg.*field = (v != 0); SaveSettingBool(key, v != 0); });
  }

  Row SegRow(const wchar_t* title, const wchar_t* caption, std::vector<std::wstring> options, float segw,
             std::function<int()> get, std::function<void(int)> set) {
    Row r;
    r.kind = Kind::Segmented; r.title = title; r.caption = caption;
    r.options = std::move(options); r.segw = segw;
    r.get = std::move(get); r.set = std::move(set);
    return r;
  }

  Row SliderRow(const wchar_t* title, const wchar_t* caption, int lo, int hi, int step, const wchar_t* suffix,
                int Settings::*field, const wchar_t* key) {
    Row r;
    r.kind = Kind::Slider; r.title = title; r.caption = caption;
    r.lo = lo; r.hi = hi; r.step = step; r.suffix = suffix;
    r.get = [this, field] { return cfg.*field; };
    r.set = [this, field, key](int v) { cfg.*field = v; SaveSettingInt(key, v); };
    return r;
  }

  Row EditRow(const wchar_t* title, const wchar_t* caption, int index) {
    Row r;
    r.kind = Kind::EditBox; r.title = title; r.caption = caption; r.edit = index;
    return r;
  }

  // A row of small action buttons on the right. `set(i)` is called when button i is clicked.
  Row ButtonsRow(const wchar_t* title, const wchar_t* caption, std::vector<std::wstring> labels,
                 std::function<void(int)> on_click) {
    Row r;
    r.kind = Kind::Buttons; r.title = title; r.caption = caption;
    r.options = std::move(labels);
    r.set = std::move(on_click);
    return r;
  }

  void BuildPages() {
    // --- Capsule ---------------------------------------------------------------------------
    {
      Section layout;
      layout.title = L"Layout";
      layout.rows.push_back(SegRow(L"Position", L"", { L"Left", L"Center", L"Right" }, 62.0f,
          [this] { return static_cast<int>(cfg.anchor); },
          [this](int v) { cfg.anchor = static_cast<PillAnchor>(v); SaveSettingValue(L"position", AnchorName(cfg.anchor)); }));
      layout.rows.push_back(SliderRow(L"Margin", L"From the taskbar edge or tray", 0, 200, 1, L" px",
                                      &Settings::margin, L"margin"));
      layout.rows.push_back(SliderRow(L"Max width", L"Widest the capsule may grow", 200, 600, 10, L" px",
                                      &Settings::max_width, L"max_width"));
      layout.rows.push_back(SegRow(L"Transport strip", L"Side the prev / play / next strip opens on",
          { L"Left", L"Right" }, 62.0f,
          [this] { return cfg.strip_left ? 0 : 1; },
          [this](int v) { cfg.strip_left = (v == 0); SaveSettingValue(L"strip_side", cfg.strip_left ? L"left" : L"right"); }));
      pages[0].push_back(std::move(layout));

      // Text font: six presets over two rows (a single row of six would not fit the window).
      Section text;
      text.title = L"Text";
      for (int row = 0; row < 2; ++row) {
        std::vector<std::wstring> labels;
        for (int i = row * 3; i < row * 3 + 3; ++i) labels.push_back(kFontPresetLabels[i]);
        const int base = row * 3;
        text.rows.push_back(SegRow(row == 0 ? L"Font" : L"", row == 0 ? L"Capsule title, artist and lyrics" : L"",
            std::move(labels), 96.0f,
            [this, base] {
              for (int i = 0; i < 3; ++i)
                if (_wcsicmp(cfg.font.c_str(), kFontPresets[base + i]) == 0) return i;
              return -1;   // another preset, or a custom name set in settings.ini
            },
            [this, base](int v) { cfg.font = kFontPresets[base + v]; SaveSettingValue(L"font", cfg.font); }));
      }
      pages[0].push_back(std::move(text));

      Section viz;
      viz.title = L"Visualizer";
      viz.rows.push_back(SegRow(L"Style", L"", { L"Off", L"Ticks", L"Wave", L"Glow" }, 54.0f,
          [this] { return static_cast<int>(cfg.visualizer); },
          [this](int v) { cfg.visualizer = static_cast<VizStyle>(v); SaveSettingValue(L"visualizer", VizName(cfg.visualizer)); }));
      viz.rows.push_back(BoolRow(L"React to audio", L"Follows what is playing (system audio loopback, nothing is recorded)",
                                 &Settings::visualizer_audio, L"visualizer_audio"));
      pages[0].push_back(std::move(viz));
    }

    // --- Behavior --------------------------------------------------------------------------
    {
      Section general;
      general.title = L"General";
      general.rows.push_back(MakeToggle(L"Start with Windows", L"",
          [this] { return autostart ? 1 : 0; },
          [this](int v) { SetAutostartEnabled(v != 0); autostart = IsAutostartEnabled(); }));
      general.rows.push_back(BoolRow(L"Hide in fullscreen", L"Follows the taskbar: hides when it does",
                                     &Settings::hide_fullscreen, L"hide_fullscreen"));
      general.rows.push_back(BoolRow(L"Collapse when paused", L"Shrinks to just the visualizer",
                                     &Settings::collapse_paused, L"collapse_paused"));
      pages[1].push_back(std::move(general));

      Section volume;
      volume.title = L"Volume";
      volume.rows.push_back(BoolRow(L"Scroll to change volume", L"Mouse wheel over the capsule",
                                    &Settings::scroll_volume, L"scroll_volume"));
      volume.rows.push_back(SliderRow(L"Step", L"Percent per wheel notch", 1, 10, 1, L"%",
                                      &Settings::scroll_step, L"scroll_step"));
      pages[1].push_back(std::move(volume));

      Section mouse;
      mouse.title = L"Mouse";
      mouse.rows.push_back(BoolRow(L"Click actions", L"Middle-click plays / pauses, right-click opens a menu",
                                   &Settings::click_actions, L"click_actions"));
      mouse.rows.push_back(BoolRow(L"Shift + scroll skips tracks", L"Scroll down = next, scroll up = previous",
                                   &Settings::shift_scroll_skip, L"shift_scroll_skip"));
      pages[1].push_back(std::move(mouse));

      Section lyrics;
      lyrics.title = L"Lyrics";
      lyrics.rows.push_back(BoolRow(L"Synced lyrics", L"Ctrl+L over the capsule toggles lyrics mode (lrclib.net)",
                                    &Settings::lyrics_enabled, L"lyrics_enabled"));
      lyrics.rows.push_back(BoolRow(L"Cache lyrics on disk", L"Replays need no network; also keeps per-song nudges",
                                    &Settings::lyrics_cache, L"lyrics_cache"));
      lyrics.rows.push_back(ButtonsRow(L"Lyrics cache", L"Open the folder or delete every cached file (nudges too)",
          { L"Open folder", L"Clear cache" },
          [this](int i) {
            if (i == 0) {
              const std::wstring dir = LyricsFetcher::CacheDir();
              CreateDirectoryW(dir.c_str(), nullptr);   // so Explorer opens even on a fresh install
              ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (i == 1) {
              const int n = LyricsFetcher::ClearDiskCache();
              // Brief confirmation in the caption is overkill; a message box keeps it obvious.
              wchar_t buf[96];
              swprintf_s(buf, L"Removed %d cached file%s.", n, n == 1 ? L"" : L"s");
              MessageBoxW(hwnd, buf, L"Island Capsule", MB_OK | MB_ICONINFORMATION);
            }
          }));
      lyrics.rows.push_back(SliderRow(L"Lyrics offset", L"All songs; positive = later (Ctrl+Left/Right = one song)",
                                      -5000, 5000, 50, L" ms", &Settings::lyrics_offset_ms, L"lyrics_offset_ms"));
      pages[1].push_back(std::move(lyrics));

      Section motion;
      motion.title = L"Motion";
      motion.rows.push_back(BoolRow(L"Seam glow", L"Accent glow when the strip opens",
                                    &Settings::seam_glow, L"seam_glow"));
      motion.rows.push_back(BoolRow(L"Overshoot", L"Springy bounce when the controls open",
                                    &Settings::overshoot, L"overshoot"));
      motion.rows.push_back(BoolRow(L"Hover scale", L"Capsule grows slightly under the mouse",
                                    &Settings::hover_scale, L"hover_scale"));
      motion.rows.push_back(BoolRow(L"Text slide", L"Text slides when the track or lyric line changes",
                                    &Settings::text_slide, L"text_slide"));
      pages[1].push_back(std::move(motion));
    }

    // --- Media -----------------------------------------------------------------------------
    {
      Section source;
      source.title = L"Source";
      source.rows.push_back(SegRow(L"Listen to", L"Which apps the capsule follows",
          { L"All apps", L"Selected only" }, 100.0f,
          [this] { return cfg.selected_only ? 1 : 0; },
          [this](int v) { cfg.selected_only = (v == 1); SaveSettingValue(L"media_source", cfg.selected_only ? L"selected" : L"all"); }));
      pages[2].push_back(std::move(source));

      Section apps;
      apps.title = L"Apps";
      apps.rows.push_back(EditRow(L"Selected apps", L"Used with \u201CSelected only\u201D, e.g. Spotify.exe, chrome.exe", 0));
      apps.rows.push_back(EditRow(L"Preferred apps", L"Picked first when several play, e.g. spotify, chrome", 1));
      pages[2].push_back(std::move(apps));
    }
  }

  // ---- setup ---------------------------------------------------------------------------------

  void MakeFonts() {
    fam = std::make_unique<gp::FontFamily>(L"Segoe UI");
    auto mk = [&](float px, int style) {
      return std::make_unique<gp::Font>(fam.get(), px * s, style, gp::UnitPixel);
    };
    f_title = mk(13.0f, gp::FontStyleRegular);
    f_cap = mk(11.5f, gp::FontStyleRegular);
    f_head = mk(12.0f, gp::FontStyleBold);
    f_tab = mk(14.0f, gp::FontStyleRegular);
    f_seg = mk(12.5f, gp::FontStyleRegular);
    f_link = mk(11.5f, gp::FontStyleRegular);
    f_link_u = mk(11.5f, gp::FontStyleUnderline);

    if (edit_font) DeleteObject(edit_font);
    edit_font = CreateFontW(-static_cast<int>(13.0f * s + 0.5f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    for (HWND e : edits)
      if (e) SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font), TRUE);
  }

  void ApplyTheme() {
    pal = MakePalette(SystemUsesDark());
    if (field_brush) DeleteObject(field_brush);
    field_brush = CreateSolidBrush(pal.fieldRef);

    BOOL dark = pal.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF cap = pal.bgRef, txt = pal.textRef;
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));    // Windows 11 only,
    DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &txt, sizeof(txt));       // harmless elsewhere
    for (HWND e : edits)
      if (e) InvalidateRect(e, nullptr, TRUE);
  }

  void CreateEdits() {
    for (int i = 0; i < 2; ++i) {
      edits[i] = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId + i)), hinst, nullptr);
      SendMessageW(edits[i], EM_LIMITTEXT, 400, 0);
    }
  }

  void SyncEdits() {
    const std::wstring texts[2] = { JoinList(cfg.selected_apps), JoinList(cfg.preferred_apps) };
    loading = true;
    for (int i = 0; i < 2; ++i) {
      if (GetFocus() == edits[i]) continue;   // never fight the user's typing
      wchar_t buf[512] = {};
      GetWindowTextW(edits[i], buf, 512);
      if (texts[i] != buf) SetWindowTextW(edits[i], texts[i].c_str());
    }
    loading = false;
  }

  void CommitEdits() {
    KillTimer(hwnd, kEditTimer);
    if (!edits_dirty) return;
    edits_dirty = false;
    static const wchar_t* const keys[2] = { L"selected_apps", L"preferred_apps" };
    for (int i = 0; i < 2; ++i) {
      wchar_t buf[512] = {};
      GetWindowTextW(edits[i], buf, 512);
      const std::wstring t = Trim(buf);
      SaveSettingValue(keys[i], t);
      (i == 0 ? cfg.selected_apps : cfg.preferred_apps) = ParseList(t);
    }
  }

  void Reload() {
    CommitEdits();
    cfg = LoadSettings();
    autostart = IsAutostartEnabled();
    SyncEdits();
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  void SetPage(int p) {
    if (p == page) return;
    CommitEdits();
    page = p;
    cfg = LoadSettings();
    autostart = IsAutostartEnabled();
    SyncEdits();
    hot = Hot{};
    scroll = 0.0f;
    for (HWND e : edits) ShowWindow(e, SW_HIDE);   // Layout() shows the ones that are in view
    Layout();
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  // ---- layout --------------------------------------------------------------------------------

  void LayoutRow(Row& r) {
    const float right = r.rect.GetRight() - 16.0f * s;
    const float cy = r.rect.Y + r.rect.Height / 2.0f;
    switch (r.kind) {
      case Kind::Toggle: {
        const float w = 40.0f * s, h = 20.0f * s;
        r.ctrl = gp::RectF(right - w, cy - h / 2.0f, w, h);
        break;
      }
      case Kind::Segmented: {
        const float total = static_cast<float>(r.options.size()) * r.segw * s, h = 28.0f * s;
        r.ctrl = gp::RectF(right - total, cy - h / 2.0f, total, h);
        break;
      }
      case Kind::Slider: {
        const float value_w = 52.0f * s, track_w = 140.0f * s;
        const float x = right - value_w - 10.0f * s - track_w;
        r.ctrl = gp::RectF(x - 8.0f * s, cy - 12.0f * s, track_w + 16.0f * s, 24.0f * s);
        break;
      }
      case Kind::Buttons: {
        // Pack buttons on the right; each is measured from its label width.
        float total = 0.0f;
        const float gap = 8.0f * s, pad_x = 12.0f * s, h = 26.0f * s;
        for (const auto& lab : r.options) {
          // Approximate width: ~7.2 logical px per character at 11.5 pt + horizontal padding.
          total += (std::max)(64.0f * s, static_cast<float>(lab.size()) * 7.2f * s + 2.0f * pad_x);
        }
        if (!r.options.empty()) total += gap * static_cast<float>(r.options.size() - 1);
        r.ctrl = gp::RectF(right - total, cy - h / 2.0f, total, h);
        break;
      }
      case Kind::EditBox:
        r.ctrl = gp::RectF(r.rect.X + 16.0f * s, r.rect.Y + 46.0f * s, r.rect.Width - 32.0f * s, 28.0f * s);
        break;
    }
  }

  void Layout() {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const float W = static_cast<float>(rc.right), H = static_cast<float>(rc.bottom);
    const float pad = 20.0f * s, tabs_h = 48.0f * s;

    for (int i = 0; i < kTabCount; ++i) tab_rect[i] = gp::RectF(i * W / kTabCount, 0.0f, W / kTabCount, tabs_h);

    flat.clear();
    view_top = tabs_h + 1.0f;
    view_bot = H - 40.0f * s;
    const float y0 = tabs_h + 8.0f * s;
    float y = y0 - scroll;
    for (Section& sec : pages[page]) {
      sec.head = gp::RectF(pad, y, W - 2.0f * pad, 24.0f * s);
      y += 26.0f * s;
      const float top = y;
      for (Row& r : sec.rows) {
        r.rect = gp::RectF(pad, y, W - 2.0f * pad, r.Height() * s);
        LayoutRow(r);
        flat.push_back(&r);
        y += r.rect.Height;
      }
      sec.card = gp::RectF(pad, top, W - 2.0f * pad, y - top);
      y += 14.0f * s;
    }
    link_rect = gp::RectF(pad, H - 34.0f * s, 150.0f * s, 26.0f * s);

    // clamp the scroll position to the content that actually exists
    max_scroll = (std::max)(0.0f, (y + scroll) - view_bot);
    if (scroll > max_scroll || scroll < 0.0f) {
      const float fixed_scroll = (std::clamp)(scroll, 0.0f, max_scroll);
      const float d = scroll - fixed_scroll;   // re-apply the correction without recursing
      scroll = fixed_scroll;
      for (Row* r : flat) { r->rect.Y += d; r->ctrl.Y += d; }
      for (Section& sec : pages[page]) { sec.head.Y += d; sec.card.Y += d; }
    }

    // The two text boxes sit inside their drawn frame.
    const int eh = static_cast<int>(20.0f * s + 0.5f);
    for (Row* r : flat) {
      if (r->kind != Kind::EditBox) continue;
      const gp::RectF& c = r->ctrl;
      const bool inside = page == 2 && c.Y >= view_top && c.GetBottom() <= view_bot;   // child windows can't be clipped
      SetWindowPos(edits[r->edit], nullptr, static_cast<int>(c.X + 10.0f * s), static_cast<int>(c.Y + (c.Height - eh) / 2.0f),
                   static_cast<int>(c.Width - 20.0f * s), eh,
                   SWP_NOZORDER | SWP_NOACTIVATE | (inside ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
  }

  // ---- scrolling -----------------------------------------------------------------------------

  gp::RectF ThumbRect() const {
    const float track = view_bot - view_top - 8.0f * s;
    const float content = track + max_scroll;
    const float th = (std::max)(28.0f * s, track * track / (std::max)(content, 1.0f));
    const float ty = view_top + 4.0f * s + (max_scroll > 0.0f ? (track - th) * scroll / max_scroll : 0.0f);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return gp::RectF(static_cast<float>(rc.right) - 10.0f * s, ty, 4.0f * s, th);
  }

  void ScrollTo(float v) {
    v = (std::clamp)(v, 0.0f, max_scroll);
    if (v == scroll) return;
    scroll = v;
    Layout();
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    hot = HitTest(static_cast<float>(pt.x), static_cast<float>(pt.y));
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  void DragThumbTo(float y) {
    const gp::RectF t = ThumbRect();
    const float track = view_bot - view_top - 8.0f * s;
    const float room = track - t.Height;
    if (room <= 0.0f) return;
    const float top = y - sb_grab - (view_top + 4.0f * s);
    ScrollTo(top / room * max_scroll);
  }

  // ---- drawing -------------------------------------------------------------------------------

  void Text(gp::Graphics& g, const std::wstring& t, const gp::Font& f, const gp::Color& c, float x, float y, float w,
            float h, gp::StringAlignment align = gp::StringAlignmentNear) {
    if (w <= 0.0f) return;
    gp::StringFormat fmt(gp::StringFormat::GenericTypographic());
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(gp::StringAlignmentCenter);
    fmt.SetTrimming(gp::StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(gp::StringFormatFlagsNoWrap);
    gp::SolidBrush b(c);
    g.DrawString(t.c_str(), -1, &f, gp::RectF(x, y, w, h), &fmt, &b);
  }

  void FillRound(gp::Graphics& g, const gp::RectF& r, float radius, const gp::Color& c) {
    gp::GraphicsPath p;
    AddRoundRect(p, r.X, r.Y, r.Width, r.Height, radius);
    gp::SolidBrush b(c);
    g.FillPath(&b, &p);
  }

  void FillPillShape(gp::Graphics& g, const gp::RectF& r, const gp::Color& c) {
    gp::GraphicsPath p;
    AddPill(p, r.X, r.Y, r.Width, r.Height);
    gp::SolidBrush b(c);
    g.FillPath(&b, &p);
  }

  void DrawToggle(gp::Graphics& g, const Row& r) {
    const bool on = r.get() != 0;
    const gp::RectF& c = r.ctrl;
    FillPillShape(g, c, on ? pal.accent : pal.off);
    const float d = c.Height - 6.0f * s;
    const float kx = on ? c.GetRight() - 3.0f * s - d : c.X + 3.0f * s;
    gp::SolidBrush knob(pal.white);
    g.FillEllipse(&knob, kx, c.Y + 3.0f * s, d, d);
  }

  void DrawSegmented(gp::Graphics& g, const Row& r, int idx) {
    const gp::RectF& c = r.ctrl;
    FillRound(g, c, 8.0f * s, pal.segBg);
    const int sel = r.get();
    const float w = r.segw * s;
    for (size_t i = 0; i < r.options.size(); ++i) {
      const gp::RectF seg(c.X + static_cast<float>(i) * w, c.Y, w, c.Height);
      const gp::RectF inner(seg.X + 2.0f * s, seg.Y + 2.0f * s, seg.Width - 4.0f * s, seg.Height - 4.0f * s);
      const bool selected = static_cast<int>(i) == sel;
      if (selected) FillRound(g, inner, 6.0f * s, pal.accent);
      else if (hot.row == idx && hot.part == static_cast<int>(i)) FillRound(g, inner, 6.0f * s, pal.segHover);
      Text(g, r.options[i], *f_seg, selected ? pal.white : pal.text, seg.X, seg.Y, seg.Width, seg.Height,
           gp::StringAlignmentCenter);
    }
  }

  void DrawSlider(gp::Graphics& g, const Row& r) {
    const gp::RectF& c = r.ctrl;
    const float tx = c.X + 8.0f * s, tw = c.Width - 16.0f * s, cy = c.Y + c.Height / 2.0f;
    const int v = r.get();
    const float frac = (std::clamp)(static_cast<float>(v - r.lo) / static_cast<float>(r.hi - r.lo), 0.0f, 1.0f);

    FillPillShape(g, gp::RectF(tx, cy - 2.0f * s, tw, 4.0f * s), pal.off);
    if (tw * frac >= 4.0f * s) FillPillShape(g, gp::RectF(tx, cy - 2.0f * s, tw * frac, 4.0f * s), pal.accent);

    const float kx = tx + tw * frac;
    const float outer = 18.0f * s, inner = 10.0f * s;
    gp::SolidBrush ring(pal.white);
    gp::Pen edge(gp::Color(40, 0, 0, 0), 1.0f);
    g.FillEllipse(&ring, kx - outer / 2.0f, cy - outer / 2.0f, outer, outer);
    g.DrawEllipse(&edge, kx - outer / 2.0f, cy - outer / 2.0f, outer, outer);
    gp::SolidBrush dot(pal.accent);
    g.FillEllipse(&dot, kx - inner / 2.0f, cy - inner / 2.0f, inner, inner);

    const float value_w = 52.0f * s;
    const float vx = r.rect.GetRight() - 16.0f * s - value_w;
    Text(g, std::to_wstring(v) + r.suffix, *f_cap, pal.sub, vx, r.rect.Y, value_w, r.rect.Height, gp::StringAlignmentFar);
  }

  void DrawEditFrame(gp::Graphics& g, const Row& r) {
    const bool focus = GetFocus() == edits[r.edit];
    gp::GraphicsPath p;
    AddRoundRect(p, r.ctrl.X + 0.5f, r.ctrl.Y + 0.5f, r.ctrl.Width - 1.0f, r.ctrl.Height - 1.0f, 6.0f * s);
    gp::SolidBrush fill(C(GetRValue(pal.fieldRef), GetGValue(pal.fieldRef), GetBValue(pal.fieldRef)));
    g.FillPath(&fill, &p);
    gp::Pen pen(focus ? pal.accent : pal.border, focus ? 1.5f : 1.0f);
    g.DrawPath(&pen, &p);
  }

  void DrawButtons(gp::Graphics& g, const Row& r, int idx) {
    const float gap = 8.0f * s, pad_x = 12.0f * s;
    float bx = r.ctrl.X;
    for (size_t b = 0; b < r.options.size(); ++b) {
      const float bw = (std::max)(64.0f * s, static_cast<float>(r.options[b].size()) * 7.2f * s + 2.0f * pad_x);
      const gp::RectF btn(bx, r.ctrl.Y, bw, r.ctrl.Height);
      const bool hover = hot.row == idx && hot.part == static_cast<int>(b);
      FillRound(g, btn, 6.0f * s, hover ? pal.segHover : pal.segBg);
      gp::Pen edge(pal.border, 1.0f);
      gp::GraphicsPath path;
      AddRoundRect(path, btn.X + 0.5f, btn.Y + 0.5f, btn.Width - 1.0f, btn.Height - 1.0f, 6.0f * s);
      g.DrawPath(&edge, &path);
      Text(g, r.options[b], *f_seg, hover ? pal.accent : pal.text, btn.X, btn.Y, btn.Width, btn.Height,
           gp::StringAlignmentCenter);
      bx += bw + gap;
    }
  }

  void DrawRow(gp::Graphics& g, const Row& r, int idx) {
    const float tx = r.rect.X + 16.0f * s;
    const bool is_edit = r.kind == Kind::EditBox;
    const float tw = (is_edit ? r.rect.GetRight() - 16.0f * s : r.ctrl.X - 12.0f * s) - tx;

    if (r.caption.empty()) {
      Text(g, r.title, *f_title, pal.text, tx, r.rect.Y, tw, is_edit ? 46.0f * s : r.rect.Height);
    } else {
      Text(g, r.title, *f_title, pal.text, tx, r.rect.Y + 8.0f * s, tw, 18.0f * s);
      Text(g, r.caption, *f_cap, pal.sub, tx, r.rect.Y + 26.0f * s, tw, 14.0f * s);
    }

    switch (r.kind) {
      case Kind::Toggle:    DrawToggle(g, r); break;
      case Kind::Segmented: DrawSegmented(g, r, idx); break;
      case Kind::Slider:    DrawSlider(g, r); break;
      case Kind::Buttons:   DrawButtons(g, r, idx); break;
      case Kind::EditBox:   DrawEditFrame(g, r); break;
    }
  }

  void Paint(HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
      gp::Graphics g(mem);
      g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
      g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
      g.Clear(pal.bg);

      // Tabs
      for (int i = 0; i < kTabCount; ++i) {
        const gp::RectF& t = tab_rect[i];
        const bool active = i == page;
        const gp::Color col = (active || hot.tab == i) ? pal.text : pal.sub;
        Text(g, kTabNames[i], *f_tab, col, t.X, t.Y, t.Width, t.Height - 4.0f * s, gp::StringAlignmentCenter);
        if (active) FillPillShape(g, gp::RectF(t.X + t.Width / 2.0f - 16.0f * s, t.GetBottom() - 5.0f * s, 32.0f * s, 3.0f * s), pal.accent);
      }
      gp::Pen divider(pal.divider, 1.0f);
      const float tabs_h = tab_rect[0].Height;
      g.DrawLine(&divider, 0.0f, tabs_h + 0.5f, static_cast<float>(W), tabs_h + 0.5f);

      // Sections (clipped to the area between the tab bar and the footer)
      g.SetClip(gp::RectF(0.0f, view_top + 1.0f, static_cast<float>(W), view_bot - view_top - 1.0f));
      int idx = 0;
      for (const Section& sec : pages[page]) {
        Text(g, sec.title, *f_head, pal.sub, sec.head.X + 4.0f * s, sec.head.Y, sec.head.Width, sec.head.Height);

        gp::GraphicsPath p;
        AddRoundRect(p, sec.card.X + 0.5f, sec.card.Y + 0.5f, sec.card.Width - 1.0f, sec.card.Height - 1.0f, 8.0f * s);
        gp::SolidBrush card(pal.card);
        g.FillPath(&card, &p);
        gp::Pen border(pal.border, 1.0f);
        g.DrawPath(&border, &p);

        for (size_t i = 0; i < sec.rows.size(); ++i, ++idx) {
          const Row& r = sec.rows[i];
          if (i > 0)
            g.DrawLine(&divider, r.rect.X + 16.0f * s, std::floor(r.rect.Y) + 0.5f, r.rect.GetRight() - 16.0f * s, std::floor(r.rect.Y) + 0.5f);
          DrawRow(g, r, idx);
        }
      }

      // Scrollbar thumb
      if (max_scroll > 0.0f) {
        const gp::RectF t = ThumbRect();
        const int a = (sb_drag || hot.sb) ? 150 : 90;
        FillRound(g, t, t.Width * 0.5f, gp::Color(static_cast<BYTE>(a), pal.sub.GetR(), pal.sub.GetG(), pal.sub.GetB()));
      }
      g.ResetClip();

      // Footer
      const float fy = static_cast<float>(H) - 40.0f * s;
      gp::SolidBrush footer_bg(pal.bg);
      g.FillRectangle(&footer_bg, 0.0f, fy, static_cast<float>(W), static_cast<float>(H) - fy);
      g.DrawLine(&divider, 0.0f, fy + 0.5f, static_cast<float>(W), fy + 0.5f);
      Text(g, L"Open settings.ini", hot.link ? *f_link_u : *f_link, pal.accent, link_rect.X, link_rect.Y, link_rect.Width, link_rect.Height);
      const float pad = 20.0f * s;
      Text(g, L"Changes apply automatically", *f_cap, pal.sub, W - pad - 200.0f * s, link_rect.Y, 200.0f * s, link_rect.Height,
           gp::StringAlignmentFar);
    }
    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);   // the DC is clipped around the EDIT children
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
  }

  // ---- input ---------------------------------------------------------------------------------

  Hot HitTest(float x, float y) const {
    Hot h;
    for (int i = 0; i < kTabCount; ++i)
      if (tab_rect[i].Contains(x, y)) { h.tab = i; return h; }
    if (link_rect.Contains(x, y)) { h.link = true; return h; }
    if (y < view_top || y >= view_bot) return h;   // tab bar / footer area: rows scrolled underneath are not clickable
    if (max_scroll > 0.0f) {
      RECT rc{};
      GetClientRect(hwnd, &rc);
      if (x >= static_cast<float>(rc.right) - 16.0f * s) { h.sb = true; return h; }
    }
    for (size_t i = 0; i < flat.size(); ++i) {
      const Row& r = *flat[i];
      if (!r.rect.Contains(x, y)) continue;
      switch (r.kind) {
        case Kind::Toggle:
          h.row = static_cast<int>(i); h.part = 0;
          break;
        case Kind::Segmented:
          if (r.ctrl.Contains(x, y)) {
            const int n = static_cast<int>(r.options.size());
            h.row = static_cast<int>(i);
            h.part = (std::clamp)(static_cast<int>((x - r.ctrl.X) / (r.segw * s)), 0, n - 1);
          }
          break;
        case Kind::Slider:
          if (r.ctrl.Contains(x, y)) { h.row = static_cast<int>(i); h.part = 0; }
          break;
        case Kind::Buttons:
          if (r.ctrl.Contains(x, y)) {
            // Walk the same packing used in DrawButtons to find which button was hit.
            const float gap = 8.0f * s, pad_x = 12.0f * s;
            float bx = r.ctrl.X;
            for (size_t b = 0; b < r.options.size(); ++b) {
              const float bw = (std::max)(64.0f * s, static_cast<float>(r.options[b].size()) * 7.2f * s + 2.0f * pad_x);
              if (x >= bx && x < bx + bw) {
                h.row = static_cast<int>(i);
                h.part = static_cast<int>(b);
                break;
              }
              bx += bw + gap;
            }
          }
          break;
        case Kind::EditBox:
          break;
      }
      return h;
    }
    return h;
  }

  void SliderTo(Row& r, float x) {
    const float tx = r.ctrl.X + 8.0f * s, tw = r.ctrl.Width - 16.0f * s;
    const float f = (std::clamp)((x - tx) / tw, 0.0f, 1.0f);
    const int steps = (std::max)(1, (r.hi - r.lo) / r.step);
    const int v = r.lo + static_cast<int>(std::lround(f * steps)) * r.step;
    if (v != r.get()) {
      r.set(v);
      InvalidateRect(hwnd, nullptr, FALSE);
    }
  }

  void OpenIni() {
    EnsureSettingsFile();
    const std::wstring path = SettingsPath();
    const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32)
      ShellExecuteW(hwnd, L"open", L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
  }

  void OnLButtonDown(float x, float y) {
    SetFocus(hwnd);   // takes focus away from a text box
    const Hot h = HitTest(x, y);
    if (h.tab >= 0) { SetPage(h.tab); return; }
    if (h.link) { OpenIni(); return; }
    if (h.sb) {
      const gp::RectF t = ThumbRect();
      sb_drag = true;
      SetCapture(hwnd);
      if (t.Contains(x, y)) sb_grab = y - t.Y;
      else { sb_grab = t.Height * 0.5f; DragThumbTo(y); }   // click on the track: jump there
      InvalidateRect(hwnd, nullptr, FALSE);
      return;
    }
    if (h.row < 0) return;

    Row& r = *flat[h.row];
    switch (r.kind) {
      case Kind::Toggle:    r.set(r.get() ? 0 : 1); break;
      case Kind::Segmented: if (r.get() != h.part) r.set(h.part); break;
      case Kind::Slider:
        drag = h.row;
        SetCapture(hwnd);
        SliderTo(r, x);
        break;
      case Kind::Buttons:
        if (h.part >= 0 && r.set) r.set(h.part);
        break;
      case Kind::EditBox:   break;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  void OnMouseMove(float x, float y) {
    if (!tracking) {
      TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, hwnd, 0 };
      TrackMouseEvent(&t);
      tracking = true;
    }
    if (sb_drag) {
      DragThumbTo(y);
      return;
    }
    if (drag >= 0 && drag < static_cast<int>(flat.size())) {
      SliderTo(*flat[drag], x);
      return;
    }
    const Hot h = HitTest(x, y);
    if (h != hot) {
      hot = h;
      InvalidateRect(hwnd, nullptr, FALSE);
    }
  }

  // ---- messages ------------------------------------------------------------------------------

  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    const float mx = static_cast<float>(static_cast<short>(LOWORD(lp)));
    const float my = static_cast<float>(static_cast<short>(HIWORD(lp)));

    switch (msg) {
      case WM_CREATE: {
        s = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
        gp::GdiplusStartupInput in;
        gp::GdiplusStartup(&gdip_token, &in, nullptr);
        gdip_ok = true;

        // same icon as the tray, in the title bar / taskbar / alt-tab
        {
          const UINT d = GetDpiForWindow(hwnd);
          icon_small = CreateAppIcon(GetSystemMetricsForDpi(SM_CXSMICON, d));
          icon_big = CreateAppIcon(GetSystemMetricsForDpi(SM_CXICON, d));
          if (icon_small) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small));
          if (icon_big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_big));
        }

        cfg = LoadSettings();
        autostart = IsAutostartEnabled();
        BuildPages();
        CreateEdits();
        ApplyTheme();
        MakeFonts();
        SyncEdits();
        Layout();
        ready = true;
        return 0;
      }

      case WM_ERASEBKGND:
        return 1;

      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (ready) Paint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
      }

      case WM_SIZE:
        if (ready) { Layout(); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;

      case WM_DPICHANGED: {
        s = static_cast<float>(HIWORD(wp)) / 96.0f;
        const RECT* sug = reinterpret_cast<const RECT*>(lp);
        MakeFonts();
        SetWindowPos(hwnd, nullptr, sug->left, sug->top, sug->right - sug->left, sug->bottom - sug->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }

      case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
          ApplyTheme();
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

      case WM_ACTIVATE:
        if (ready && LOWORD(wp) != WA_INACTIVE && !edits_dirty) Reload();   // pick up manual edits of the ini
        return 0;

      case WM_LBUTTONDOWN:
        OnLButtonDown(mx, my);
        return 0;

      case WM_LBUTTONUP:
        if (sb_drag) {
          sb_drag = false;
          ReleaseCapture();
          hot = HitTest(mx, my);
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (drag >= 0) {
          drag = -1;
          ReleaseCapture();
          hot = HitTest(mx, my);
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

      case WM_CAPTURECHANGED:
        drag = -1;
        sb_drag = false;
        return 0;

      case WM_MOUSEMOVE:
        OnMouseMove(mx, my);
        return 0;

      case WM_MOUSELEAVE:
        tracking = false;
        if (hot.Interactive() && drag < 0) {
          hot = Hot{};
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

      case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
          POINT pt{};
          GetCursorPos(&pt);
          ScreenToClient(hwnd, &pt);
          if (drag >= 0 || HitTest(static_cast<float>(pt.x), static_cast<float>(pt.y)).Interactive()) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
          }
        }
        break;

      case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (id >= kEditId && id < kEditId + 2) {
          if (code == EN_CHANGE && !loading) {
            edits_dirty = true;
            SetTimer(hwnd, kEditTimer, 500, nullptr);   // save shortly after the user stops typing
          } else if (code == EN_KILLFOCUS) {
            CommitEdits();
            InvalidateRect(hwnd, nullptr, FALSE);
          } else if (code == EN_SETFOCUS) {
            InvalidateRect(hwnd, nullptr, FALSE);
          }
        }
        return 0;
      }

      case WM_TIMER:
        if (wp == kEditTimer) CommitEdits();
        return 0;

      case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, pal.textRef);
        SetBkColor(dc, pal.fieldRef);
        return reinterpret_cast<LRESULT>(field_brush);
      }

      case WM_MOUSEWHEEL: {
        const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / static_cast<float>(WHEEL_DELTA);
        ScrollTo(scroll - notches * 56.0f * s);
        return 0;
      }

      case WM_KEYDOWN: {
        const float page_px = view_bot - view_top - 24.0f * s;
        switch (wp) {
          case VK_ESCAPE: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
          case VK_DOWN:   ScrollTo(scroll + 40.0f * s); break;
          case VK_UP:     ScrollTo(scroll - 40.0f * s); break;
          case VK_NEXT:   ScrollTo(scroll + page_px); break;
          case VK_PRIOR:  ScrollTo(scroll - page_px); break;
          case VK_HOME:   ScrollTo(0.0f); break;
          case VK_END:    ScrollTo(max_scroll); break;
          default: break;
        }
        return 0;
      }

      case WM_CLOSE:
        CommitEdits();
        DestroyWindow(hwnd);
        return 0;

      default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
};

Ui* g_ui = nullptr;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Ui* ui = reinterpret_cast<Ui*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (m == WM_NCCREATE) {
    ui = static_cast<Ui*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
    ui->hwnd = h;
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
  }
  if (!ui) return DefWindowProcW(h, m, w, l);

  if (m == WM_NCDESTROY) {
    const LRESULT r = DefWindowProcW(h, m, w, l);
    SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    if (g_ui == ui) g_ui = nullptr;
    delete ui;
    return r;
  }
  return ui->OnMessage(m, w, l);
}

} // namespace

void OpenSettingsWindow(HINSTANCE hinst) {
  if (g_ui && g_ui->hwnd && IsWindow(g_ui->hwnd)) {
    if (IsIconic(g_ui->hwnd)) ShowWindow(g_ui->hwnd, SW_RESTORE);
    SetForegroundWindow(g_ui->hwnd);
    return;
  }

  EnsureSettingsFile();

  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // The capsule icon embedded in the exe (resource id 1, see resources/island_capsule.rc.in); generic icon if absent.
    wc.hIcon = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), LR_SHARED));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    registered = true;
  }

  // Size for the monitor the user is on, centered in its work area.
  POINT pt{};
  GetCursorPos(&pt);
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  UINT dpi_x = 96, dpi_y = 96;
  GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);

  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
  RECT r{ 0, 0, MulDiv(kClientW, static_cast<int>(dpi_x), 96), MulDiv(kClientH, static_cast<int>(dpi_x), 96) };
  AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi_x);
  const int w = r.right - r.left, h = r.bottom - r.top;

  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
  const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;

  Ui* ui = new Ui();
  ui->hinst = hinst;
  g_ui = ui;

  HWND hwnd = CreateWindowExW(0, kClass, L"Island Capsule Settings", style, x, y, w, h, nullptr, nullptr, hinst, ui);
  if (!hwnd) {
    if (g_ui == ui) { g_ui = nullptr; delete ui; }   // (WM_NCDESTROY already freed it otherwise)
    return;
  }
  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
}

void CloseSettingsWindow() {
  if (g_ui && g_ui->hwnd && IsWindow(g_ui->hwnd)) SendMessageW(g_ui->hwnd, WM_CLOSE, 0, 0);
}

} // namespace island
