// Pill window: GDI+ rendering into a premultiplied 32-bit DIB, shown with
// UpdateLayeredWindow so the rounded edges are properly anti-aliased.
// Polish pass: the transport strip is its own slightly shorter capsule tucked UNDER the main pill,
// no scaling anywhere (everything is pixel-aligned), cover pre-downsampled to its exact on-screen
// size, integer-snapped text.

#include "ui/gdip.hpp"
#include <ShellScalingApi.h>
#include <shellapi.h>

#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "audio/spectrum.hpp"
#include "audio/volume.hpp"
#include "lyrics/lrc.hpp"
#include "theme/accent.hpp"
#include "theme/brushes.hpp"
#include "ui/dwtext.hpp"
#include "ui/marquee.hpp"
#include "ui/motion.hpp"
#include "ui/pill.hpp"
#include "ui/settings_window.hpp"
#include "ui/transport.hpp"
#include "ui/visualizer.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace gp = Gdiplus;

namespace island {

namespace {

constexpr wchar_t kClassName[] = L"IslandCapsulePill";

// Logical (96-dpi) metrics
constexpr double kPillH     = 42.0;
constexpr double kMinW      = 120.0;
constexpr double kArtSize   = 32.0;
constexpr double kPadLeft   = 10.0;  // generous left margin so the cover clears the round end
constexpr double kGap       = 8.0;
constexpr double kPadRight  = 16.0;   // right padding when the visualizer is off
constexpr double kPadRightViz = 12.0; // right padding when the visualizer is on
constexpr double kVizGap    = 8.0;    // text -> visualizer slot
constexpr double kTitlePx   = 14.0;   // whole pixels render crisper than fractional sizes
constexpr double kArtistPx  = 12.0;
constexpr double kArtRadius = 6.5;
constexpr double kFadeW     = 14.0;   // marquee edge fade
constexpr double kHoverHold = 0.25;   // seconds the strip stays after the mouse leaves
constexpr double kWinPad    = 8.0;    // transparent margin around the content (hover scale room)
constexpr double kHoverGrow = 0.03;   // the whole capsule (text included) scales up by this much on hover
constexpr double kHudMinAvail = 112.0; // text-area width the volume HUD needs (speaker + bar + percent)
constexpr double kSlideDur  = 0.32;   // text slide on track change
constexpr double kSlideOff  = 14.0;   // logical px travelled by the slide
constexpr double kArtFade   = 0.35;   // cover crossfade
constexpr double kIdleHide  = 1.5;    // seconds without a session before the capsule fades out
constexpr double kVizFrame  = 0.030;  // visualizer-only redraw interval (~30 fps)
constexpr double kVizMargin = 0.20;   // visualizer repaint rect = slot + this fraction of the slot's long side (glow halo), + 2 px
constexpr double kEnvPoll   = 1.0;    // DPI / theme safety-net re-read (WM_SETTINGCHANGE etc. trigger it sooner)
constexpr unsigned kGuardWaitMs = 4;  // z-order guard cadence while the cursor is over the taskbar (see GuardZ)
constexpr double kHotHold = 0.5;      // keep the guard on this long after the cursor leaves the taskbar area
constexpr double kHotAboveDp = 340.0; // how far above the taskbar counts as "hot" (thumbnail / jump-list height), logical px
constexpr unsigned kIdleWaitMs = 50;  // loop sleep while nothing animates (also the worst-case fullscreen reaction time)
constexpr int kNudgeStepMs = 100;      // Ctrl+Left / Ctrl+Right step for the per-song lyrics timing nudge
constexpr double kNudgeShow = 1.2;     // seconds the nudge indicator stays on the capsule
constexpr double kLyricSlide = 0.30;   // lyric line transition (seconds)
constexpr double kLyricSlideOff = 8.0; // logical px a lyric line travels while changing
constexpr double kLyricMaxW = 900.0;   // lyrics capsule may grow past max_width so the WHOLE line fits (no scrolling)
constexpr double kUpFillet = 7.0;      // concave fillet where the control tab melts into the capsule
constexpr double kLyricMinFrac = 0.70; // lyrics capsule never narrower than this fraction of max_width (no breathing)

// "Follow the taskbar": the capsule lives on the taskbar, so it is shown exactly while the taskbar is
// really visible. A fullscreen app / game / video makes Windows hide the taskbar or put that app's window
// over it. Instead of asking "who is on top of one pixel of the taskbar" (which any flyout, thumbnail or
// switching animation can fool), this looks at the FOREGROUND window itself: is it a real app window
// (not the shell: desktop, Task View, Start, the taskbar all live in explorer's process) that covers the
// whole monitor the taskbar is on, and is not just an ordinary maximized window (those keep their caption)?
//   Yes     -> covered (`who` = class of that window, for the log)
//   No      -> the taskbar is showing
//   Unknown -> mid-switch (no foreground window right now): keep the current state
enum class Cover { No, Yes, Unknown };

Cover TaskbarCover(HWND self, HWND taskbar, POINT probe, wchar_t* who, int who_len) {
  if (who && who_len > 0) who[0] = L'\0';
  if (!taskbar) return Cover::Unknown;
  if (!IsWindowVisible(taskbar)) {
    if (who) wcsncpy_s(who, who_len, L"(taskbar hidden)", _TRUNCATE);
    return Cover::Yes;
  }
  if (!MonitorFromPoint(probe, MONITOR_DEFAULTTONULL)) {   // auto-hidden taskbar slid off screen
    if (who) wcsncpy_s(who, who_len, L"(taskbar off screen)", _TRUNCATE);
    return Cover::Yes;
  }

  HWND fg = GetForegroundWindow();
  if (!fg || fg == self) return Cover::Unknown;

  DWORD fg_pid = 0, tb_pid = 0;
  GetWindowThreadProcessId(fg, &fg_pid);
  GetWindowThreadProcessId(taskbar, &tb_pid);
  if (fg_pid == tb_pid) return Cover::No;                   // shell UI is never "a fullscreen app"
  if (!IsWindowVisible(fg) || IsIconic(fg)) return Cover::No;

  HMONITOR tb_mon = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
  if (!tb_mon || MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) != tb_mon) return Cover::No;   // other screen

  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  RECT r{};
  if (!GetMonitorInfoW(tb_mon, &mi) || !GetWindowRect(fg, &r)) return Cover::Unknown;
  const bool covers = r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
                      r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
  if (!covers) return Cover::No;

  // An ordinary maximized window (e.g. with an auto-hidden taskbar) still has its caption.
  const LONG style = GetWindowLongW(fg, GWL_STYLE);
  if (IsZoomed(fg) && (style & WS_CAPTION) == WS_CAPTION) return Cover::No;

  if (who) GetClassNameW(fg, who, who_len);
  return Cover::Yes;
}

// True when the taskbar was raised above the capsule (it does that when you switch apps).
bool TaskbarAbove(HWND self, HWND taskbar) {
  for (HWND w = GetWindow(self, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
    if (w == taskbar) return true;
  return false;
}

// One text hint for measuring AND drawing capsule text (title, artist, lyrics, volume HUD). The capsule is a
// translucent layered surface, so ClearType is not an option; GridFit however snaps every glyph advance to whole
// pixels, which with Segoe UI Variable gives uneven spacing ("mi nd") and soft stems. Plain grayscale AA keeps the
// true advances. Switch to TextRenderingHintAntiAliasGridFit here to go back.
constexpr gp::TextRenderingHint kCapsuleTextHint = gp::TextRenderingHintAntiAlias;
constexpr UINT kCapsuleTextContrast = 6;   // 0..12 (default 4): higher = crisper, slightly bolder grayscale text

struct Fonts {
  std::unique_ptr<gp::FontFamily> family;
  std::unique_ptr<gp::Font> title;
  std::unique_ptr<gp::Font> artist;
  std::unique_ptr<gp::StringFormat> fmt;   // no wrap, no trimming, no clip

  // Measured once per Fonts instance (Fonts are rebuilt on a DPI change, which drops these too). GetHeight() and
  // MeasureString() are slow in GDI+ and used to run every frame.
  float title_h = -1.0f, artist_h = -1.0f;   // line heights
  float vol_reserve = -1.0f;                  // width of "100" in the title font (volume HUD)
  int vol_num_pct = -1;                       // last volume percent measured ...
  float vol_num_w = 0.0f;                     // ... and its width
  std::unique_ptr<DwText> dw;   // DirectWrite renderer; null => everything falls back to GDI+ text

  float TitleH(gp::Graphics& g) {
    if (title_h < 0.0f) title_h = dw ? dw->LineHeight(false) : title->GetHeight(&g);
    return title_h;
  }
  float ArtistH(gp::Graphics& g) {
    if (artist_h < 0.0f) artist_h = dw ? dw->LineHeight(true) : artist->GetHeight(&g);
    return artist_h;
  }
  float Width(gp::Graphics& g, const std::wstring& s, bool is_artist);   // defined after MeasureWidth

  explicit Fonts(double scale, const std::wstring& family_name) {
    family.reset(new gp::FontFamily(family_name.c_str()));
    if (!family->IsAvailable()) family.reset(new gp::FontFamily(L"Segoe UI Variable Text"));
    if (!family->IsAvailable()) family.reset(new gp::FontFamily(L"Segoe UI"));
    if (!family->IsAvailable()) family.reset(gp::FontFamily::GenericSansSerif()->Clone());
    // Whole-pixel sizes: fractional sizes make GDI+ smear stems across pixel rows.
    const gp::REAL tpx = static_cast<gp::REAL>((std::max)(1L, std::lround(kTitlePx * scale)));
    const gp::REAL apx = static_cast<gp::REAL>((std::max)(1L, std::lround(kArtistPx * scale)));
    title.reset(new gp::Font(family.get(), tpx, gp::FontStyleBold, gp::UnitPixel));
    artist.reset(new gp::Font(family.get(), apx, gp::FontStyleRegular, gp::UnitPixel));
    fmt.reset(gp::StringFormat::GenericTypographic()->Clone());
    fmt->SetTrimming(gp::StringTrimmingNone);
    fmt->SetFormatFlags(gp::StringFormatFlagsNoWrap | gp::StringFormatFlagsNoClip);

    dw = std::make_unique<DwText>();
    if (!dw->Init(static_cast<float>(tpx), static_cast<float>(apx), family_name)) dw.reset();   // soft failure: GDI+ text
  }
};

float MeasureWidth(gp::Graphics& g, const std::wstring& s, const gp::Font& f,
                   const gp::StringFormat& fmt) {
  if (s.empty()) return 0.0f;
  gp::RectF box;
  g.MeasureString(s.c_str(), static_cast<INT>(s.size()), &f, gp::PointF(0, 0), &fmt, &box);
  return box.Width;
}

float Fonts::Width(gp::Graphics& g, const std::wstring& s, bool is_artist) {
  if (dw) return dw->Width(s, is_artist);
  return MeasureWidth(g, s, is_artist ? *artist : *title, *fmt);
}

inline uint32_t ScalePremul(uint32_t p, float f) {
  const uint32_t a = static_cast<uint32_t>(((p >> 24) & 0xFF) * f);
  const uint32_t r = static_cast<uint32_t>(((p >> 16) & 0xFF) * f);
  const uint32_t g = static_cast<uint32_t>(((p >> 8) & 0xFF) * f);
  const uint32_t b = static_cast<uint32_t>((p & 0xFF) * f);
  return (a << 24) | (r << 16) | (g << 8) | b;
}

// Fade the left/right edges of a premultiplied bitmap (marquee edge fade).
void ApplyEdgeFade(gp::Bitmap& bmp, int w, int h, int left_w, int right_w) {
  if (left_w <= 0 && right_w <= 0) return;
  gp::BitmapData bd;
  gp::Rect r(0, 0, w, h);
  if (bmp.LockBits(&r, gp::ImageLockModeRead | gp::ImageLockModeWrite,
                   PixelFormat32bppPARGB, &bd) != gp::Ok)
    return;
  for (int y = 0; y < h; ++y) {
    auto* row = reinterpret_cast<uint32_t*>(static_cast<BYTE*>(bd.Scan0) + static_cast<ptrdiff_t>(y) * bd.Stride);
    for (int x = 0; x < left_w && x < w; ++x)
      row[x] = ScalePremul(row[x], (static_cast<float>(x) + 0.5f) / static_cast<float>(left_w));
    for (int x = 0; x < right_w && x < w; ++x)
      row[w - 1 - x] = ScalePremul(row[w - 1 - x], (static_cast<float>(x) + 0.5f) / static_cast<float>(right_w));
  }
  bmp.UnlockBits(&bd);
}

uint32_t HashText(const std::wstring& s) {
  uint32_t h = 2166136261u;
  for (wchar_t c : s) { h ^= static_cast<uint32_t>(c); h *= 16777619u; }
  return h ? h : 1u;
}

bool ArtValid(const ArtImage* img) {
  return img && img->w > 0 && img->h > 0 &&
         img->bgra.size() == static_cast<size_t>(img->w) * static_cast<size_t>(img->h) * 4;
}

// Cover pre-scaled to the exact on-screen pixel size with a box (area-average) filter.
// GDI+ bicubic does not prefilter big reductions, which is what made the cover look soft/aliased.
struct ScaledArt {
  std::shared_ptr<const ArtImage> src;   // keeps the source alive so the pointer key cannot be reused
  int px = 0;
  std::vector<uint8_t> pixels;           // backing store for `bmp`
  std::unique_ptr<gp::Bitmap> bmp;
  std::unique_ptr<gp::TextureBrush> brush;   // cached brush for the fully opaque case (see DrawArtFill)
  float brush_x = 0.0f, brush_y = 0.0f;

  bool Matches(const std::shared_ptr<const ArtImage>& s, int size) const {
    return bmp && src == s && px == size;
  }

  void Build(const std::shared_ptr<const ArtImage>& s, int size) {
    brush.reset();   // references `bmp`: must go first
    bmp.reset();
    src = s;
    px = size;
    if (!ArtValid(s.get()) || size <= 0) return;

    const ArtImage& in = *s;
    pixels.assign(static_cast<size_t>(size) * size * 4, 0);
    const double r = static_cast<double>(in.w) / size;   // source pixels per dest pixel
    for (int y = 0; y < size; ++y) {
      const double fy0 = y * r, fy1 = (y + 1) * r;
      const int iy0 = static_cast<int>(std::floor(fy0));
      const int iy1 = (std::min)(in.h, static_cast<int>(std::ceil(fy1)));
      for (int x = 0; x < size; ++x) {
        const double fx0 = x * r, fx1 = (x + 1) * r;
        const int ix0 = static_cast<int>(std::floor(fx0));
        const int ix1 = (std::min)(in.w, static_cast<int>(std::ceil(fx1)));
        double acc[4] = { 0, 0, 0, 0 };
        double wsum = 0.0;
        for (int sy = iy0; sy < iy1; ++sy) {
          const double wy = (std::min)(fy1, sy + 1.0) - (std::max)(fy0, static_cast<double>(sy));
          const uint8_t* row = in.bgra.data() + static_cast<size_t>(sy) * in.w * 4;
          for (int sx = ix0; sx < ix1; ++sx) {
            const double wx = (std::min)(fx1, sx + 1.0) - (std::max)(fx0, static_cast<double>(sx));
            const double w = wx * wy;
            const uint8_t* p = row + static_cast<size_t>(sx) * 4;
            acc[0] += p[0] * w; acc[1] += p[1] * w; acc[2] += p[2] * w; acc[3] += p[3] * w;
            wsum += w;
          }
        }
        uint8_t* d = pixels.data() + (static_cast<size_t>(y) * size + x) * 4;
        if (wsum > 0.0)
          for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>((std::min)(255.0, acc[c] / wsum + 0.5));
      }
    }
    bmp.reset(new gp::Bitmap(size, size, size * 4, PixelFormat32bppPARGB, pixels.data()));
  }
};

// Fills `clip` with the cover (anti-aliased rounded edge, unlike a GDI+ clip region), placed at
// whole-pixel (x, y). `alpha` is the crossfade opacity.
void DrawArtFill(gp::Graphics& g, ScaledArt& art, const gp::GraphicsPath& clip, float x, float y, float alpha) {
  if (!art.bmp || alpha <= 0.002f) return;

  // Steady state (no crossfade running): one TextureBrush is built per cover and reused every frame.
  if (alpha >= 0.998f) {
    if (!art.brush || art.brush_x != x || art.brush_y != y) {
      art.brush = std::make_unique<gp::TextureBrush>(art.bmp.get(), gp::Rect(0, 0, art.px, art.px));
      art.brush->SetWrapMode(gp::WrapModeTileFlipXY);
      art.brush->TranslateTransform(x, y);
      art.brush_x = x;
      art.brush_y = y;
    }
    g.FillPath(art.brush.get(), &clip);
    return;
  }

  // Crossfade: the opacity changes every frame, so the brush is rebuilt (short, ~0.35 s).
  gp::ImageAttributes ia;
  if (alpha < 0.998f) {
    gp::ColorMatrix cm{};
    for (int i = 0; i < 5; ++i) cm.m[i][i] = 1.0f;
    cm.m[3][3] = alpha;
    ia.SetColorMatrix(&cm, gp::ColorMatrixFlagsDefault, gp::ColorAdjustTypeBitmap);
  }
  gp::TextureBrush tb(art.bmp.get(), gp::Rect(0, 0, art.px, art.px), &ia);
  tb.SetWrapMode(gp::WrapModeTileFlipXY);
  tb.TranslateTransform(x, y);
  g.FillPath(&tb, &clip);
}

} // namespace

// ---------------------------------------------------------------------------

struct PillWindow::Impl {
  ULONG_PTR gdip_token = 0;
  bool gdip_ok = false;
  HWND hwnd = nullptr;
  HINSTANCE hinst = nullptr;
  bool class_registered = false;

  Settings cfg;
  std::function<void(PillCommand)> on_command;
  std::function<void(const std::wstring&)> on_log;
  void Logf(const std::wstring& line) const {
    if (on_log) on_log(line);
  }

  std::shared_ptr<const MediaInfo> media = std::make_shared<const MediaInfo>();
  bool content_dirty = true;

  // environment
  int dpi = 96;
  bool dark = true;   // the glass is ALWAYS the dark variant: the album tint, not the Windows theme, sets the look

  // Cached environment. The DPI used to be re-read every tick (plus FindWindow + a registry query for the
  // theme, ~20x/s while idle). Now it is re-read on an event (env_dirty) or once per kEnvPoll as a safety net.
  HWND tb_cached = nullptr;     // Shell_TrayWnd, re-found only when the window is gone (explorer restart)
  bool env_dirty = true;        // WM_SETTINGCHANGE / WM_DISPLAYCHANGE / WM_DPICHANGED / WM_THEMECHANGED
  double env_since = 99.0;      // seconds since the DPI was read
  int env_dpi = 96;
  bool shown = false;       // window currently visible
  bool laid_out = false;    // Layout() has run at least once
  bool hidden = false;      // faded out (no media / fullscreen)

  // text
  std::wstring title_text, artist_text;
  std::wstring last_title, last_artist;
  bool dim = false;
  bool have_text = false;
  float title_w = 0.0f, artist_w = 0.0f;
  bool title_over = false, artist_over = false;
  Marquee title_mq, artist_mq;
  std::unique_ptr<Fonts> fonts;
  int fonts_dpi = 0;
  std::wstring fonts_family;   // resolved family the current Fonts were built with

  // skip-direction text slide (Phase 5)
  std::wstring old_title, old_artist;
  float old_tw = 0.0f, old_aw = 0.0f;
  bool old_dim = false;
  bool slide_active = false;
  double slide_t = 1.0;
  int slide_dir = 0;
  int pending_dir = 0;
  double pending_age = 99.0;

  // cover + crossfade (Phase 5)
  std::shared_ptr<const ArtImage> art_cur, art_prev;
  double art_t = 1.0;
  ScaledArt scaled_cur, scaled_prev;   // exact-size copies of art_cur / art_prev

  // album color (Phase 3)
  std::shared_ptr<const ArtImage> accent_src;
  Rgb acc_cur{ 150.0f, 150.0f, 160.0f }, acc_tgt{ 150.0f, 150.0f, 160.0f };
  float str_cur = 0.0f, str_tgt = 0.0f;
  double art_missing = 0.0;
  GlassStyle style = ComposeGlass(true, Rgb{ 150.0f, 150.0f, 160.0f }, 0.0f);
  // inputs `style` was composed from: it is only recomposed when one of them changed
  bool style_valid = false;
  Rgb style_acc{};
  float style_str = 0.0f;

  // visualizer (Phase 4)
  Visualizer viz;
  AudioSpectrum audio;   // WASAPI loopback + FFT; its thread only exists while the visualizer needs it

  // lyrics mode (src/lyrics)
  std::shared_ptr<const LyricsSnapshot> lyr;
  std::wstring media_track_id;      // TrackId(*media), cached
  bool lyrics_user_on = false;      // Ctrl+L / lyrics_mode: the user wants lyrics
  bool lyric_ok = false;            // lyrics for THIS track are ready and the player reports a timeline
  double lyr_mix = 0.0;             // 0 = title / artist, 1 = lyric line (crossfade)
  double lyr_target_w = 0.0;        // capsule width for the current lyric line
  int lyr_idx = -2;                 // current line (-1 = before the first line, -2 = none yet)
  std::wstring lyr_text, lyr_old;
  float lyr_text_w = 0.0f, lyr_old_w = 0.0f;
  bool lyr_over = false;
  int lyr_next_ms = -1;             // ms until the next lyric line is due (-1 = unknown / paused): lets the loop wake on time
  // Text measuring is the expensive part of a line change: one persistent off-screen surface + a width cache.
  HDC measure_dc = nullptr;
  std::unique_ptr<gp::Graphics> measure_g;
  std::unordered_map<std::wstring, float> lyr_wcache;   // line text -> width (cleared when the font / dpi changes)
  Marquee lyr_mq;
  double lyr_t = 1.0;               // line transition 0..1
  float def_mul = 1.0f;             // opacity of the title / artist block (1 - lyric mix)

  // Per-song timing nudge: Ctrl+Left / Ctrl+Right (+-kNudgeStepMs) over the capsule while lyrics are shown,
  // Ctrl+Down resets it. effective offset = global lyrics_offset_ms + track_off_ms.
  std::function<void(const std::wstring&, int)> offset_handler;   // (track id, new per-track nudge) -> saved by the loader
  std::wstring nudge_track_id;      // the track track_off_ms belongs to
  int track_off_ms = 0;
  double nudge_hold = 0.0;          // seconds the "Lyrics +300 ms" indicator stays
  double nudge_t = 0.0;             // indicator fade 0..1 (the lyric line fades out as it fades in)
  std::wstring nudge_str;
  float nudge_w = -1.0f;            // measured width of nudge_str (-1 = measure on next draw)

  // geometry / animation
  TransportStrip strip;
  double cur_w = 160.0;
  double target_w = 160.0;
  Spring exp_sp;           // strip expansion, may overshoot slightly
  Spring up_sp;            // lyrics mode: the transport panel rising ABOVE the capsule (0..1, may overshoot)
  Spring hov_sp;           // hover scale 0..1
  Spring col_sp;           // 0 = full capsule, 1 = collapsed to the visualizer (paused)
  float content_a = 1.0f;  // art / text opacity while collapsing
  double paused_for = 0.0; // seconds the session has been paused (debounces track-change gaps)

  // fullscreen auto-hide
  bool fullscreen = false;

  // scroll-wheel volume HUD
  SystemVolume sysvol;
  int wheel_acc = 0;       // accumulated wheel delta (120 = one notch)
  float vol_level = 0.0f;  // real level after the last scroll
  float vol_disp = 0.0f;   // smoothed level shown by the bar
  bool vol_muted = false;
  double vol_hold = 0.0;   // seconds the HUD stays after the last scroll
  double vol_t = 0.0;      // HUD fade 0..1 (text fades out as it fades in)
  double hold = 0.0;
  bool hover = false;
  bool tracking = false;
  int pressed = -1;
  double vis = 0.0;        // 0 = faded out, 1 = fully visible (startup + idle hide)
  double idle_for = 99.0;
  double seam = 1.0;
  bool seam_active = false;
  bool was_want = false;     // side strip was wanted last frame

  // frame pacing
  bool major_anim = false;
  bool viz_anim = false;
  bool pending_viz = false;
  double since_render = 1.0;
  double since_zorder = 1.0;

  int pill_h = 36, pill_w = 160;
  int win_w = 160, win_h = 36, pad_px = 0;
  int pill_off_x = 0, strip_x = 0, clip_x = 0, clip_w = 0, extra_px = 0;
  int pill_off_y = 0;      // capsule's top edge inside the window (pad + the part of the up panel showing)
  int up_extra_px = 0;     // height of the up panel showing above the capsule (0 when closed)
  int win_x = 0, win_y = 0;

  LARGE_INTEGER qpc_freq{};
  LARGE_INTEGER qpc_last{};
  bool have_qpc = false;

  double Scale() const { return dpi / 96.0; }
  int Dir() const { return cfg.strip_left ? -1 : 1; }   // +1 = strip on the right of the main capsule

  // Paused => collapse to just the visualizer; hovering reopens it so the controls are usable.
  bool CollapseWanted() const {
    return cfg.collapse_paused && viz.Enabled() && media->has_session && !media->is_playing &&
           paused_for > 0.6 && !(hover || hold > 0.0 || pressed >= 0);
  }
  double CollapsedW() const {
    return (std::max)(static_cast<double>(pill_h),
                      static_cast<double>(viz.SlotWidth()) + 2.0 * std::lround(kPadRightViz * Scale()));
  }
  double Expand() const { return exp_sp.x; }
  double ExpandUp() const { return up_sp.x; }
  double MaxW() const { return static_cast<double>((std::clamp)(cfg.max_width, 200, 600)); }

  // Distance from the pill's right edge to where text must stop.
  int RightReservePx() const {
    const double s = Scale();
    if (viz.Enabled())
      return static_cast<int>(std::lround((kVizGap + kPadRightViz) * s)) + viz.SlotWidth();
    return static_cast<int>(std::lround(kPadRight * s));
  }

  // ---- text ---------------------------------------------------------------

  void ResolveText() {
    const MediaInfo& m = *media;
    std::wstring nt, na;
    bool nd = false;
    if (!m.has_session) {
      nt = L"No media";
      nd = true;
    } else {
      nt = m.title.empty() ? L"Unknown title" : m.title;
      na = m.artist;
    }

    const bool changed = nt != last_title || na != last_artist;
    if (changed && have_text) {
      // Keep the outgoing text for the slide / crossfade.
      old_title = title_text;
      old_artist = artist_text;
      old_tw = title_w;
      old_aw = artist_w;
      old_dim = dim;
      slide_dir = pending_age < 3.0 ? pending_dir : 0;
      pending_dir = 0;
      slide_active = true;
      slide_t = 0.0;
    }

    title_text = std::move(nt);
    artist_text = std::move(na);
    dim = nd;

    if (changed) {
      title_mq.Reset();
      artist_mq.Reset();
      last_title = title_text;
      last_artist = artist_text;
      viz.SetSeed(HashText(title_text + L"\x1f" + artist_text));
    }
    have_text = true;
  }

  // ---- cover ---------------------------------------------------------------

  void UpdateArt(bool snap) {
    if (!media->has_session) {
      if (art_cur) {
        art_prev = art_cur;
        art_cur.reset();
        art_t = snap ? 1.0 : 0.0;
        if (snap) art_prev.reset();
      }
      return;
    }
    // A momentarily missing cover keeps the old one (see Animate); only a new cover swaps.
    if (media->art && media->art != art_cur) {
      art_prev = art_cur;
      art_cur = media->art;
      art_t = snap ? 1.0 : 0.0;
      if (snap) art_prev.reset();
    }
  }

  // ---- album color ----------------------------------------------------------

  void SetAccentTarget(const Accent& a) {
    acc_tgt = a.color;
    str_tgt = a.strength;
  }

  void UpdateAccentSource(bool snap) {
    const MediaInfo& m = *media;
    if (!m.has_session) {
      if (accent_src || str_tgt != 0.0f) SetAccentTarget(Accent{});
      accent_src.reset();
      art_missing = 0.0;
    } else if (m.art && m.art != accent_src) {
      accent_src = m.art;
      if (m.art->accent_ready) {   // computed on the media worker thread, right after the decode
        Accent a;
        a.color = { m.art->accent_r, m.art->accent_g, m.art->accent_b };
        a.strength = m.art->accent_strength;
        SetAccentTarget(a);
      } else {
        SetAccentTarget(ExtractAccent(*m.art));   // fallback: art that did not come from the worker
      }
      art_missing = 0.0;
    }
    if (snap) {
      acc_cur = acc_tgt;
      str_cur = str_tgt;
    }
  }

  // Measure text, decide the target width, strip geometry. `snap` = no easing.
  void Layout(bool snap) {
    const double s = Scale();
    pill_h = static_cast<int>(std::lround(kPillH * s));
    pad_px = static_cast<int>(std::lround(kWinPad * s));

    viz.SetStyle(cfg.visualizer);
    viz.SetReducedMotion(cfg.reduced_motion);
    viz.SetAudioSource(cfg.visualizer_audio ? &audio : nullptr);
    viz.Layout(s, pill_h);
    viz.SetState(media->has_session, media->is_playing);

    ResolveText();
    UpdateAccentSource(snap);
    UpdateArt(snap);

    const std::wstring want_family = ResolveFontFamily(cfg.font);
    if (!fonts || fonts_dpi != dpi || fonts_family != want_family) {
      fonts.reset(new Fonts(s, want_family));
      fonts_dpi = dpi;
      fonts_family = want_family;
    }

    HDC sdc = GetDC(nullptr);
    if (sdc) {
      gp::Graphics g(sdc);
      g.SetTextRenderingHint(kCapsuleTextHint);
      title_w = fonts->Width(g, title_text, false);
      artist_w = fonts->Width(g, artist_text, true);
      ReleaseDC(nullptr, sdc);
    }

    const double fixed = (kPadLeft + kArtSize + kGap) * s + RightReservePx();
    double w = fixed + std::ceil((std::max)(title_w, artist_w)) + 2.0;
    w = (std::min)((std::max)(w, kMinW * s), MaxW() * s);
    target_w = std::round(w);

    const double avail_target = target_w - fixed;
    title_over = title_w > avail_target + 0.5;
    artist_over = artist_w > avail_target + 0.5;

    lyr_wcache.clear();      // fonts / dpi changed: cached widths are stale
    RefreshLyricMetrics();   // fonts / dpi / max_width may have changed

    strip.Layout(s, pill_h, Dir() > 0);
    strip.LayoutUp(s);
    if (snap) {
      cur_w = target_w;
      if (media->has_session && !media->is_playing) paused_for = 99.0;
      col_sp.Snap(CollapseWanted() ? 1.0 : 0.0);
    }
  }

  // ---- lyrics -------------------------------------------------------------

  bool LyricsWanted() const { return cfg.lyrics_enabled && lyrics_user_on; }

  void ReleaseMeasure() {
    measure_g.reset();   // GDI+ objects must go before GdiplusShutdown
    if (measure_dc) { DeleteDC(measure_dc); measure_dc = nullptr; }
  }

  float MeasureLyric(const std::wstring& str) {
    if (!fonts || str.empty()) return 0.0f;
    if (const auto it = lyr_wcache.find(str); it != lyr_wcache.end()) return it->second;   // repeated lines (chorus)
    if (!measure_g) {
      if (!measure_dc) measure_dc = CreateCompatibleDC(nullptr);
      if (!measure_dc) return 0.0f;
      measure_g = std::make_unique<gp::Graphics>(measure_dc);
      measure_g->SetTextRenderingHint(kCapsuleTextHint);
    }
    const float w = fonts->Width(*measure_g, str, false);
    if (lyr_wcache.size() > 512) lyr_wcache.clear();   // bounded: a few songs' worth
    lyr_wcache.emplace(str, w);
    return w;
  }

  // Width the capsule wants for the current lyric line + whether the line needs the marquee.
  void RefreshLyricMetrics() {
    if (lyr_text.empty()) return;
    const double s = Scale();
    lyr_text_w = MeasureLyric(lyr_text);
    const double fixed = (kPadLeft + kArtSize + kGap) * s + RightReservePx();
    const double hi = (std::max)(MaxW(), kLyricMaxW) * s;   // full line always fits (practically)
    const double lo = (std::min)(hi, (std::max)(kMinW, MaxW() * kLyricMinFrac) * s);
    lyr_target_w = std::round((std::clamp)(fixed + std::ceil(lyr_text_w) + 2.0, lo, hi));
    lyr_over = false;   // lyrics never scroll
  }

  void SetLyricLine(const std::wstring& text, bool animate) {
    if (animate && !lyr_text.empty()) {
      lyr_old = lyr_text;
      lyr_old_w = lyr_text_w;
      lyr_t = 0.0;
    } else {
      lyr_old.clear();
      lyr_t = 1.0;
    }
    lyr_text = text;
    lyr_mq.Reset();
    RefreshLyricMetrics();
  }

  int EffectiveOffsetMs() const { return (std::clamp)(cfg.lyrics_offset_ms + track_off_ms, -5000, 5000); }

  // Ctrl+Left / Ctrl+Right (step_ms = -/+kNudgeStepMs) or Ctrl+Down (step_ms = 0: reset this song's nudge).
  // Only the per-song part changes; the global lyrics_offset_ms stays in the settings window.
  void NudgeLyrics(int step_ms) {
    if (!lyric_ok || nudge_track_id.empty()) return;
    const int g = cfg.lyrics_offset_ms;
    const int want = step_ms == 0 ? (std::clamp)(g, -5000, 5000) : (std::clamp)(g + track_off_ms + step_ms, -5000, 5000);
    const int old = track_off_ms;
    track_off_ms = want - g;
    if (track_off_ms != old && offset_handler) offset_handler(nudge_track_id, track_off_ms);
    nudge_str = L"Lyrics " + std::wstring(want > 0 ? L"+" : L"") + std::to_wstring(want) + L" ms";
    nudge_w = -1.0f;
    nudge_hold = kNudgeShow;
    Logf(L"[lyrics] timing " + std::to_wstring(want) + L" ms (this song " + std::to_wstring(track_off_ms) + L" ms)\r\n");
    static_dirty = true;
    major_anim = true;
  }

  // Picks the lyric line for the current playback position (the position is only used here; it is never
  // drawn). Returns true when something visible changed.
  bool UpdateLyrics() {
    bool changed = false;
    lyr_next_ms = -1;   // recomputed below while a synced line list is playing
    const LyricsSnapshot* L = lyr.get();
    const bool ok = LyricsWanted() && fonts && L && L->state == LyricsState::Ready && L->lines &&
                    !L->lines->empty() && media->has_session && media->has_timeline &&
                    L->track_id == media_track_id;
    if (ok != lyric_ok) {
      lyric_ok = ok;
      changed = true;
    }
    if (ok) {
      // The per-song nudge belongs to the track the snapshot is for: pick up the saved value when the track changes.
      if (L->track_id != nudge_track_id) {
        nudge_track_id = L->track_id;
        track_off_ms = L->track_offset_ms;
      }
      // positive offset = lyrics appear later (global setting + this song's nudge)
      const int64_t pos = media->PositionNowMs() - EffectiveOffsetMs();
      const int idx = FindLyricLineHinted(*L->lines, pos, lyr_idx);   // O(1) while the song just plays on
      lyr_next_ms = -1;
      if (media->is_playing && static_cast<size_t>(idx + 1) < L->lines->size()) {
        const double rate = (std::max)(0.1, media->playback_rate);
        lyr_next_ms = static_cast<int>(
            (std::min)(1000.0, static_cast<double>((*L->lines)[static_cast<size_t>(idx + 1)].ms - pos) / rate));
      }
      if (idx != lyr_idx) {
        const bool animate = lyr_idx != -2 && lyr_mix > 0.5;
        lyr_idx = idx;
        std::wstring t = idx >= 0 ? (*L->lines)[static_cast<size_t>(idx)].text : std::wstring();
        if (t.empty()) t = L"\u266A";   // instrumental gap / intro
        if (t != lyr_text) SetLyricLine(t, animate);
        changed = true;
      }
    } else if (lyr_idx != -2 && lyr_mix <= 0.003) {
      // faded out completely: forget the line so the next song starts clean
      lyr_idx = -2;
      lyr_text.clear();
      lyr_old.clear();
      lyr_target_w = 0.0;
      lyr_over = false;
      lyr_next_ms = -1;
    }
    return changed;
  }

  // ---- animation ----------------------------------------------------------

  double FrameDt() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!have_qpc) {
      QueryPerformanceFrequency(&qpc_freq);
      qpc_last = now;
      have_qpc = true;
      return 0.0;
    }
    double dt = static_cast<double>(now.QuadPart - qpc_last.QuadPart) / static_cast<double>(qpc_freq.QuadPart);
    qpc_last = now;
    return (std::min)((std::max)(dt, 0.0), 0.1);
  }

  // Everything except the visualizer. Returns true if a redraw is needed.
  bool Animate(double dt, bool want_hidden) {
    bool changed = false;
    const double s = Scale();
    const bool reduced = cfg.reduced_motion;
    const bool springy = cfg.overshoot && !reduced;
    const bool grows = cfg.hover_scale && !reduced;
    pending_age += dt;

    // fade in at startup / out when idle
    {
      const double vt = want_hidden ? 0.0 : 1.0;
      const double vd = vt - vis;
      if (std::fabs(vd) > 0.004) {
        vis += vd * (1.0 - std::exp(-dt * 9.0));
        changed = true;
      } else if (vd != 0.0) {
        vis = vt;
        changed = true;
      }
    }

    // autofit width ease
    // While the volume HUD is up the capsule is at least wide enough to hold it, so a short
    // title (narrow capsule) still gets a visible volume bar instead of blank space.
    double want_w = target_w;
    if (lyr_target_w > 0.0) want_w += (lyr_target_w - target_w) * (std::clamp)(lyr_mix, 0.0, 1.0);
    if (vol_hold > 0.0 || vol_t > 0.01) {
      const double hud_w = (kPadLeft + kArtSize + kGap) * s + RightReservePx() + kHudMinAvail * s;
      want_w = (std::max)(want_w, hud_w);
    }
    const double wd = want_w - cur_w;
    if (std::fabs(wd) > 0.5) {
      cur_w += wd * (1.0 - std::exp(-dt * 14.0));
      changed = true;
    } else if (wd != 0.0) {
      cur_w = want_w;
      changed = true;
    }

    // side-expand (spring)
    if (hold > 0.0) {
      hold -= dt;
      changed = true;   // keep ticking until the hold expires, so the collapse starts on time
    }
    // Hover opens the transport controls: sideways normally, UPWARD above the capsule while lyrics are shown.
    // (If the mode flips while hovering, one closes as the other opens.)
    const bool want_any = media->has_session && (hover || hold > 0.0 || pressed >= 0);
    const bool want = want_any && !lyric_ok;        // side strip
    const bool want_up = want_any && lyric_ok;      // panel above
    constexpr double kExpandK = 380.0;
    changed |= exp_sp.Step(dt, want ? 1.0 : 0.0, kExpandK, !springy ? CriticalDamping(kExpandK) : 28.0);
    changed |= up_sp.Step(dt, want_up ? 1.0 : 0.0, kExpandK, !springy ? CriticalDamping(kExpandK) : 28.0);
    constexpr double kHoverK = 300.0;
    changed |= hov_sp.Step(dt, (grows && (hover || pressed >= 0)) ? 1.0 : 0.0, kHoverK,
                           CriticalDamping(kHoverK) * 0.85);

    // paused: collapse toward the middle to just the visualizer; playing again opens it back out
    if (media->has_session && !media->is_playing) {
      paused_for += dt;
      if (paused_for < 0.7) changed = true;   // keep ticking until the debounce elapses
    } else {
      paused_for = 0.0;
    }
    constexpr double kColK = 190.0;
    changed |= col_sp.Step(dt, CollapseWanted() ? 1.0 : 0.0, kColK, CriticalDamping(kColK));
    content_a = (std::clamp)(1.0f - static_cast<float>(col_sp.x) * 1.7f, 0.0f, 1.0f);

    if (Expand() < 0.01 && ExpandUp() < 0.01 && !hover) strip.Reset();

    // seam glow: crawls out from the join when the strip starts to open
    if (want && !was_want) {
      seam = 0.0;
      seam_active = cfg.seam_glow && !reduced;
    }
    was_want = want;
    if (seam_active) {
      seam += dt / 0.6;
      if (seam >= 1.0) {
        seam = 1.0;
        seam_active = false;
      }
      changed = true;
    }

    // a live session without a cover: keep the old cover + tint for a grace period
    if (media->has_session && !media->art && (accent_src || art_cur)) {
      art_missing += dt;
      if (art_missing > 1.5) {
        accent_src.reset();
        SetAccentTarget(Accent{});
        if (art_cur) {
          art_prev = art_cur;
          art_cur.reset();
          art_t = 0.0;
        }
        changed = true;
      }
    } else if (media->art) {
      art_missing = 0.0;
    }

    // album accent crossfade (~0.4 s)
    {
      const float k = static_cast<float>(1.0 - std::exp(-dt * 7.0));
      const float dr = acc_tgt.r - acc_cur.r, dg = acc_tgt.g - acc_cur.g, db = acc_tgt.b - acc_cur.b;
      const float ds = str_tgt - str_cur;
      const float diff = (std::max)({ std::fabs(dr), std::fabs(dg), std::fabs(db), std::fabs(ds) * 100.0f });
      if (diff > 0.4f) {
        acc_cur.r += dr * k; acc_cur.g += dg * k; acc_cur.b += db * k;
        str_cur += ds * k;
        changed = true;
      } else if (diff > 0.0f) {
        acc_cur = acc_tgt;
        str_cur = str_tgt;
        changed = true;
      }
    }

    // cover crossfade
    if (art_t < 1.0) {
      art_t += dt / kArtFade;
      if (art_t >= 1.0) {
        art_t = 1.0;
        art_prev.reset();
      }
      changed = true;
    }

    // text slide
    if (slide_active) {
      slide_t += dt / kSlideDur;
      if (slide_t >= 1.0) {
        slide_t = 1.0;
        slide_active = false;
      }
      changed = true;
    }

    // volume HUD: shown while scrolling, then fades back to the title / artist
    if (vol_hold > 0.0) {
      vol_hold -= dt;
      changed = true;
    }
    {
      const double vt = vol_hold > 0.0 ? 1.0 : 0.0;
      const double vd = vt - vol_t;
      if (std::fabs(vd) > 0.004) {
        vol_t += vd * (1.0 - std::exp(-dt * 14.0));
        changed = true;
      } else if (vd != 0.0) {
        vol_t = vt;
        changed = true;
      }
      const float ld = vol_level - vol_disp;
      if (std::fabs(ld) > 0.002f) {
        vol_disp += ld * static_cast<float>(1.0 - std::exp(-dt * 22.0));
        changed = true;
      } else {
        vol_disp = vol_level;
      }
    }

    // timing nudge indicator: shown briefly after Ctrl+Left / Right / Down, then the lyric line fades back
    if (nudge_hold > 0.0) {
      nudge_hold -= dt;
      changed = true;
    }
    {
      const double nt = nudge_hold > 0.0 ? 1.0 : 0.0;
      const double nd = nt - nudge_t;
      if (std::fabs(nd) > 0.004) {
        nudge_t += nd * (1.0 - std::exp(-dt * 14.0));
        changed = true;
      } else if (nd != 0.0) {
        nudge_t = nt;
        changed = true;
      }
    }

    // lyrics <-> title/artist crossfade, lyric line transition
    {
      const double lt = lyric_ok ? 1.0 : 0.0;
      const double ld = lt - lyr_mix;
      if (std::fabs(ld) > 0.004) {
        lyr_mix += ld * (1.0 - std::exp(-dt * 10.0));
        changed = true;
      } else if (ld != 0.0) {
        lyr_mix = lt;
        changed = true;
      }
      if (lyr_t < 1.0) {
        lyr_t += dt / kLyricSlide;
        if (lyr_t >= 1.0) {
          lyr_t = 1.0;
          lyr_old.clear();
        }
        changed = true;
      }
      changed |= lyr_mq.Update(dt, lyr_over && lyr_mix > 0.5, lyr_text_w, s);
    }
    def_mul = static_cast<float>((std::clamp)(1.0 - lyr_mix, 0.0, 1.0));

    // marquee
    changed |= title_mq.Update(dt, title_over && lyr_mix < 0.5, title_w, s);
    changed |= artist_mq.Update(dt, artist_over && lyr_mix < 0.5, artist_w, s);

    // hover / press highlights
    changed |= strip.Update(dt);
    return changed;
  }

  // ---- drawing ------------------------------------------------------------

  TransportState CurrentTransport() const {
    TransportState t;
    t.playing = media->is_playing;
    t.can_prev = media->can_prev;
    t.can_play_pause = media->can_play_pause;
    t.can_next = media->can_next;
    return t;
  }

  // Alpha (0..255) applied to a cached DirectWrite run at draw time.
  static void AlphaAttrs(gp::ImageAttributes& ia, float alpha255) {
    const float a = (std::clamp)(alpha255 / 255.0f, 0.0f, 1.0f);
    gp::ColorMatrix cm = {
      1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.0f, a,    0.0f,
      0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
    ia.SetColorMatrix(&cm, gp::ColorMatrixFlagsDefault, gp::ColorAdjustTypeBitmap);
  }

  static int Ch(float v) { return static_cast<int>((std::clamp)(v, 0.0f, 255.0f) + 0.5f); }

  // Draws `str` with DirectWrite (cached bitmap + DrawImage) at a whole-pixel origin. False => not available,
  // the caller draws with GDI+ instead.
  bool DrawDwRun(gp::Graphics& g, const std::wstring& str, bool is_artist, const Rgb& ink, float alpha255,
                 float x, float y) {
    if (!fonts || !fonts->dw || alpha255 <= 0.5f) return false;
    const DwText::Run* run = fonts->dw->Get(str, is_artist, Ch(ink.r), Ch(ink.g), Ch(ink.b));
    if (!run || !run->bmp) return false;
    gp::ImageAttributes ia;
    AlphaAttrs(ia, alpha255);
    const gp::GraphicsState st = g.Save();
    if (RootScale() == 1.0f) g.SetInterpolationMode(gp::InterpolationModeNearestNeighbor);   // exact 1:1 copy
    g.DrawImage(run->bmp, gp::RectF(std::round(x), std::round(y), static_cast<float>(run->w), static_cast<float>(run->h)),
                0.0f, 0.0f, static_cast<float>(run->w), static_cast<float>(run->h), gp::UnitPixel, &ia);
    g.Restore(st);
    return true;
  }

  // One line of text. `x` is where the text starts (may be slid), `clip_x` the fixed
  // left edge of the text area. `mq` null => static (outgoing text during a slide).
  void DrawLine(gp::Graphics& g, const std::wstring& str, const gp::Font& font, const Rgb& ink,
                float alpha, float x, float clip_x, float y, float avail, float line_h,
                float text_w, bool overflow, const Marquee* mq) {
    if (str.empty() || avail <= 1.0f || alpha <= 0.5f) return;
    // Whole-pixel origin: text drawn at a fractional position is resampled and looks blurry.
    x = std::round(x);
    y = std::round(y);

    const bool is_artist = fonts && &font == fonts->artist.get();
    const DwText::Run* run = (fonts && fonts->dw) ? fonts->dw->Get(str, is_artist, Ch(ink.r), Ch(ink.g), Ch(ink.b)) : nullptr;
    gp::ImageAttributes ia;
    if (run) AlphaAttrs(ia, alpha);
    const bool exact = RootScale() == 1.0f;

    const gp::GraphicsState st = g.Save();
    g.SetClip(gp::RectF(clip_x, y, avail, line_h), gp::CombineModeIntersect);

    if (!overflow || !mq) {
      if (run) {
        if (exact) g.SetInterpolationMode(gp::InterpolationModeNearestNeighbor);
        g.DrawImage(run->bmp, gp::RectF(x, y, static_cast<float>(run->w), static_cast<float>(run->h)), 0.0f, 0.0f,
                    static_cast<float>(run->w), static_cast<float>(run->h), gp::UnitPixel, &ia);
      } else {
        gp::SolidBrush brush(Col(ink, alpha));
        g.DrawString(str.c_str(), -1, &font, gp::RectF(x, y, text_w + 4.0f, line_h), fonts->fmt.get(), &brush);
      }
    } else {
      // Marquee: render into a persistent off-screen strip (re-created only when its size changes), fade the
      // edges, composite.
      const int tw = static_cast<int>(std::ceil(avail));
      const int th = static_cast<int>(std::ceil(line_h));
      MarqueeScratch& sc = mq_scratch[mq == &artist_mq ? 1 : 0];
      if (!sc.bmp || sc.w != tw || sc.h != th) {
        sc.Release();
        sc.bmp = std::make_unique<gp::Bitmap>(tw, th, PixelFormat32bppPARGB);
        sc.w = tw;
        sc.h = th;
      }
      {
        // Graphics goes out of scope before ApplyEdgeFade locks the bits (same order as before).
        gp::Graphics tg(sc.bmp.get());
        tg.SetTextRenderingHint(kCapsuleTextHint);
        tg.SetTextContrast(kCapsuleTextContrast);
        tg.Clear(gp::Color(0, 0, 0, 0));
        const float off = std::round(mq->Offset());
        const float loop = std::round(mq->Loop());
        if (run) {
          tg.SetInterpolationMode(gp::InterpolationModeNearestNeighbor);
          const float rw = static_cast<float>(run->w), rh = static_cast<float>(run->h);
          tg.DrawImage(run->bmp, gp::RectF(-off, 0.0f, rw, rh), 0.0f, 0.0f, rw, rh, gp::UnitPixel, &ia);
          tg.DrawImage(run->bmp, gp::RectF(-off + loop, 0.0f, rw, rh), 0.0f, 0.0f, rw, rh, gp::UnitPixel, &ia);
        } else {
          gp::SolidBrush brush(Col(ink, alpha));
          tg.DrawString(str.c_str(), -1, &font, gp::RectF(-off, 0.0f, text_w + 4.0f, static_cast<float>(th)),
                        fonts->fmt.get(), &brush);
          tg.DrawString(str.c_str(), -1, &font, gp::RectF(-off + loop, 0.0f, text_w + 4.0f, static_cast<float>(th)),
                        fonts->fmt.get(), &brush);
        }
      }
      const int fade = static_cast<int>(std::lround(kFadeW * Scale()));
      const int left_fade = (std::min)(fade, static_cast<int>(std::round(mq->Offset())));
      ApplyEdgeFade(*sc.bmp, tw, th, left_fade, fade);
      g.DrawImage(sc.bmp.get(), gp::RectF(x, y, static_cast<float>(tw), static_cast<float>(th)),
                  0.0f, 0.0f, static_cast<gp::REAL>(tw), static_cast<gp::REAL>(th), gp::UnitPixel);
    }
    g.Restore(st);
  }

  void DrawTextBlock(gp::Graphics& g, float tx, float avail, float H, float xoff, float alpha,
                     const std::wstring& title, const std::wstring& artist, float tw, float aw,
                     bool tover, bool aover, bool is_dim, const Marquee* tmq, const Marquee* amq) {
    const float th = fonts->TitleH(g);
    const float ah = fonts->ArtistH(g);
    const float hud = 1.0f - static_cast<float>(vol_t);   // text gives way to the volume bar
    const float ta = (is_dim ? 150.0f : style.title_a) * alpha * content_a * hud * def_mul;
    const float aa = style.sub_a * alpha * content_a * hud * def_mul;

    if (artist.empty()) {
      const float ty = std::round((H - th) / 2.0f);
      DrawLine(g, title, *fonts->title, style.title, ta, tx + xoff, tx, ty, avail, th, tw, tover, tmq);
    } else {
      const float ty = std::round((H - (th + ah)) / 2.0f);
      DrawLine(g, title, *fonts->title, style.title, ta, tx + xoff, tx, ty, avail, th, tw, tover, tmq);
      DrawLine(g, artist, *fonts->artist, style.sub, aa, tx + xoff, tx, ty + th, avail, ah, aw, aover, amq);
    }
  }

  // Main capsule surface (0,0 = pill top-left).
  // Width of the control tab above the capsule: its own width (3 buttons), clamped to the capsule's flat top.
  float UpTabWidth() const {
    return (std::min)(static_cast<float>(strip.UpWidth()), static_cast<float>(pill_w - pill_h));
  }

  // Capsule + control tab as a single closed outline (pill-local coords, capsule at 0,0; the tab rises to y = -U).
  // Concave fillets where tab meets capsule make it read as one shape, with no seam line.
  void BuildUnionPath(gp::GraphicsPath& path, float W, float H, float U, float tw) const {
    const float R = H * 0.5f;
    const float xl = (W - tw) * 0.5f, xr = xl + tw;
    const float f = (std::min)({ static_cast<float>(kUpFillet * Scale()), U, xl - R });
    const float rp = (std::max)(1.0f, (std::min)({ static_cast<float>(strip.UpHeight()) * 0.5f, U * 0.5f, tw * 0.5f }));
    if (f > 0.5f) path.AddArc(xl - 2 * f, -2 * f, 2 * f, 2 * f, 90.0f, -90.0f);   // fillet (left)
    path.AddArc(xl, -U, 2 * rp, 2 * rp, 180.0f, 90.0f);                             // tab top-left
    path.AddArc(xr - 2 * rp, -U, 2 * rp, 2 * rp, 270.0f, 90.0f);                    // tab top-right
    if (f > 0.5f) path.AddArc(xr, -2 * f, 2 * f, 2 * f, 180.0f, -90.0f);          // fillet (right)
    path.AddArc(W - H, 0.0f, H, H, 270.0f, 180.0f);                             // right end
    path.AddArc(0.0f, 0.0f, H, H, 90.0f, 180.0f);                               // left end
    path.CloseFigure();
  }

  void DrawSurface(gp::Graphics& g, float W, float H) {
    if (W <= 1.0f) return;
    const float U = static_cast<float>(up_extra_px);
    const bool merged = U >= 2.0f && W - H > 8.0f;   // control tab open: one merged shape
    const float tw = merged ? UpTabWidth() : 0.0f;
    const float uk = merged ? U : 0.0f;

    // Geometry is rebuilt only when the capsule size / tab changed (not for viz-only or colour-only frames).
    SurfaceCache& c = surf;
    if (!c.body || c.W != W || c.H != H || c.U != uk || c.tw != tw || c.merged != merged) {
      c.body = std::make_unique<gp::GraphicsPath>();
      c.rim_path.reset();
      if (merged) {
        BuildUnionPath(*c.body, W, H, U, tw);
      } else {
        AddPill(*c.body, 0.0f, 0.0f, W, H);
        c.rim_path = std::make_unique<gp::GraphicsPath>();
        AddPill(*c.rim_path, 0.5f, 0.5f, W - 1.0f, H - 1.0f);
      }
      c.fill = std::make_unique<gp::LinearGradientBrush>(gp::PointF(0.0f, 0.0f), gp::PointF(W, 0.0f),
                                                         Col(style.fill, style.fill_a),
                                                         Col(style.fill2, style.fill2_a));
      c.rim = std::make_unique<gp::Pen>(Col(style.rim, style.rim_a), 1.0f);
      if (merged) c.rim->SetAlignment(gp::PenAlignmentInset);
      c.W = W; c.H = H; c.U = uk; c.tw = tw; c.merged = merged;
    }
    // The album tint animates: colours are updated in place.
    c.fill->SetLinearColors(Col(style.fill, style.fill_a), Col(style.fill2, style.fill2_a));
    c.rim->SetColor(Col(style.rim, style.rim_a));

    g.FillPath(c.fill.get(), c.body.get());
    g.DrawPath(c.rim.get(), merged ? c.body.get() : c.rim_path.get());
  }

  // Accent glow crawling out from the join, clipped to the strip capsule (strip coordinates).
  void DrawSeam(gp::Graphics& g, float alpha) {
    const float W = static_cast<float>(strip.Width());
    const float ov = static_cast<float>(strip.Overlap());
    const float sh = strip.SurfaceHeight();
    const float y0 = strip.SurfaceInset();
    if (W <= 0.0f) return;

    const float t = static_cast<float>(EaseOutCubic(seam));
    const float env = std::sin(3.14159265f * static_cast<float>(seam));
    // join = where the strip meets the pill edge (inner end), glow travels to the far end
    const float cx = Dir() > 0 ? ov + (W - ov) * t : (W - ov) * (1.0f - t);
    const float r = W * 0.75f;

    gp::GraphicsPath body;
    AddPill(body, 0.0f, y0, W, sh);
    const gp::GraphicsState st = g.Save();
    g.SetClip(&body, gp::CombineModeIntersect);

    gp::GraphicsPath blob;
    blob.AddEllipse(cx - r, y0 - sh * 0.5f, r * 2.0f, sh * 2.0f);
    gp::PathGradientBrush pb(&blob);
    pb.SetCenterColor(Col(style.viz, 115.0f * env, alpha));
    gp::Color edge = Col(style.viz, 0.0f, 0.0f);
    INT count = 1;
    pb.SetSurroundColors(&edge, &count);
    g.FillPath(&pb, &blob);
    g.Restore(st);
  }

  // The current lyric line (single line, title font), crossfaded in over the title / artist block.
  void DrawLyricLine(gp::Graphics& g, float tx, float avail, float H) {
    if (lyr_mix <= 0.003 || lyr_text.empty() || !fonts) return;
    const float s = static_cast<float>(Scale());
    const float th = fonts->TitleH(g);
    const float hud = (1.0f - static_cast<float>(vol_t)) * (1.0f - static_cast<float>(nudge_t));
    const float base = style.title_a * content_a * hud * static_cast<float>(lyr_mix);
    const float ty = std::round((H - th) / 2.0f);
    const float te = lyr_t < 1.0 ? static_cast<float>(EaseOutCubic(lyr_t)) : 1.0f;
    const float off = (cfg.reduced_motion || !cfg.text_slide) ? 0.0f : static_cast<float>(kLyricSlideOff) * s;
    // Lines are centered in the text area (between the cover and the visualizer) and never scroll.
    auto cx = [&](float w) { return tx + (std::max)(0.0f, (avail - w) * 0.5f); };
    if (lyr_t < 1.0 && !lyr_old.empty())   // outgoing line drifts up and fades
      DrawLine(g, lyr_old, *fonts->title, style.title, base * (1.0f - te), cx(lyr_old_w), tx, ty - off * te, avail,
               th, lyr_old_w, false, nullptr);
    DrawLine(g, lyr_text, *fonts->title, style.title, base * te, cx(lyr_text_w), tx, ty + off * (1.0f - te), avail,
             th, lyr_text_w, false, nullptr);
  }

  // "Lyrics +300 ms": replaces the lyric line for a moment after a timing nudge.
  void DrawNudge(gp::Graphics& g, float tx, float avail, float H) {
    const float a = static_cast<float>(nudge_t) * static_cast<float>(lyr_mix);
    if (a <= 0.01f || nudge_str.empty() || !fonts) return;
    if (nudge_w < 0.0f) nudge_w = fonts->Width(g, nudge_str, false);
    const float th = fonts->TitleH(g);
    const float hud = 1.0f - static_cast<float>(vol_t);
    const float ty = std::round((H - th) / 2.0f);
    const float x = tx + (std::max)(0.0f, (avail - nudge_w) * 0.5f);
    DrawLine(g, nudge_str, *fonts->title, style.title, style.title_a * content_a * hud * a, x, tx, ty, avail, th,
             nudge_w, false, nullptr);
  }

  // Volume HUD that replaces the text area while scrolling: [speaker] [====----] [45]
  void DrawVolume(gp::Graphics& g, float tx, float avail, float H) {
    const float a = static_cast<float>(vol_t);
    const float s = static_cast<float>(Scale());
    if (a <= 0.01f || avail < 60.0f * s) return;

    const float cy = H * 0.5f;
    const float right = tx + avail;

    // percent, right aligned
    const int pct = vol_muted ? 0 : static_cast<int>(std::lround((std::clamp)(vol_disp, 0.0f, 1.0f) * 100.0f));
    const std::wstring num = std::to_wstring(pct);
    Fonts& fo = *fonts;
    if (fo.vol_reserve < 0.0f) fo.vol_reserve = fo.Width(g, L"100", false);
    if (fo.vol_num_pct != pct) {   // the number only changes when the level does, not every frame
      fo.vol_num_w = fo.Width(g, num, false);
      fo.vol_num_pct = pct;
    }
    const float reserve = fo.vol_reserve;
    const float nw = fo.vol_num_w;
    const float nh = fo.TitleH(g);
    if (!DrawDwRun(g, num, false, style.title, style.title_a * a, std::round(right - nw), std::round(cy - nh * 0.5f))) {
      gp::SolidBrush nb(Col(style.title, style.title_a, a));
      g.DrawString(num.c_str(), -1, fonts->title.get(),
                   gp::PointF(std::round(right - nw), std::round(cy - nh * 0.5f)), fonts->fmt.get(), &nb);
    }

    // speaker glyph
    const float ix = std::round(tx);
    {
      gp::SolidBrush ink(Col(style.ink, 235.0f, a));
      const gp::PointF cone[6] = { { ix, cy - 2.4f * s }, { ix + 3.0f * s, cy - 2.4f * s },
                                   { ix + 6.6f * s, cy - 5.4f * s }, { ix + 6.6f * s, cy + 5.4f * s },
                                   { ix + 3.0f * s, cy + 2.4f * s }, { ix, cy + 2.4f * s } };
      g.FillPolygon(&ink, cone, 6);

      gp::Pen pen(Col(style.ink, 235.0f, a), (std::max)(1.2f, 1.4f * s));
      pen.SetStartCap(gp::LineCapRound);
      pen.SetEndCap(gp::LineCapRound);
      if (vol_muted || pct == 0) {
        g.DrawLine(&pen, ix + 8.4f * s, cy - 2.6f * s, ix + 12.6f * s, cy + 2.6f * s);
        g.DrawLine(&pen, ix + 8.4f * s, cy + 2.6f * s, ix + 12.6f * s, cy - 2.6f * s);
      } else {
        const float r1 = 2.6f * s, r2 = 5.2f * s, ax = ix + 7.4f * s;
        g.DrawArc(&pen, ax - r1, cy - r1, r1 * 2.0f, r1 * 2.0f, -45.0f, 90.0f);
        if (pct > 33) g.DrawArc(&pen, ax - r2, cy - r2, r2 * 2.0f, r2 * 2.0f, -45.0f, 90.0f);
      }
    }

    // bar
    const float x0 = ix + 14.5f * s + 6.0f * s;
    const float x1 = right - reserve - 8.0f * s;
    if (x1 - x0 < 16.0f * s) return;
    const float th = (std::max)(3.0f, std::round(4.0f * s));
    const float by = std::round(cy - th * 0.5f);

    gp::GraphicsPath track;
    AddRoundRect(track, x0, by, x1 - x0, th, th * 0.5f);
    gp::SolidBrush tb(Col(style.ink, 46.0f, a));
    g.FillPath(&tb, &track);

    const float lvl = vol_muted ? 0.0f : (std::clamp)(vol_disp, 0.0f, 1.0f);
    if (lvl > 0.004f) {
      const float fw = (std::max)(th, (x1 - x0) * lvl);
      gp::GraphicsPath done;
      AddRoundRect(done, x0, by, fw, th, th * 0.5f);
      gp::SolidBrush fb(Col(style.viz, style.viz_a, a));
      g.FillPath(&fb, &done);
    }
  }

  // Pill content only (cover, text, visualizer); the surface is drawn by DrawSurface.
  void DrawPill(gp::Graphics& g) {
    const double s = Scale();
    const float W = static_cast<float>(pill_w);
    const float H = static_cast<float>(pill_h);

    // Album art, crossfaded on track change. Whole-pixel size + position, pre-downsampled copy.
    const int art_px = (std::max)(1, static_cast<int>(std::lround(kArtSize * s)));
    const float art = static_cast<float>(art_px);
    const float art_x = static_cast<float>(std::lround(kPadLeft * s));
    const float art_y = std::floor((H - art) / 2.0f);
    {
      const float art_r = static_cast<float>(kArtRadius * s);
      ArtGfxCache& ac = art_gfx;
      if (!ac.clip || ac.px != art_px || ac.x != art_x || ac.y != art_y || ac.r != art_r) {
        ac.clip = std::make_unique<gp::GraphicsPath>();
        AddRoundRect(*ac.clip, art_x, art_y, art, art, art_r);
        ac.px = art_px; ac.x = art_x; ac.y = art_y; ac.r = art_r;
      }
      const gp::GraphicsPath& clip = *ac.clip;

      const gp::Color ph_col = Col(style.ink, 34.0f, content_a);
      if (!ac.placeholder) ac.placeholder = std::make_unique<gp::SolidBrush>(ph_col);
      else ac.placeholder->SetColor(ph_col);
      g.FillPath(ac.placeholder.get(), &clip);

      if (art_cur && !scaled_cur.Matches(art_cur, art_px)) scaled_cur.Build(art_cur, art_px);
      if (art_prev && !scaled_prev.Matches(art_prev, art_px)) scaled_prev.Build(art_prev, art_px);
      if (!art_cur) scaled_cur = ScaledArt{};
      if (!art_prev) scaled_prev = ScaledArt{};

      const float te = art_t < 1.0 ? static_cast<float>(EaseOutCubic(art_t)) : 1.0f;
      if (art_prev) DrawArtFill(g, scaled_prev, clip, art_x, art_y, (art_cur ? 1.0f : 1.0f - te) * content_a);
      if (art_cur)  DrawArtFill(g, scaled_cur, clip, art_x, art_y, te * content_a);

      const gp::Color st_col = Col(style.ink, 40.0f, content_a);
      if (!ac.stroke) ac.stroke = std::make_unique<gp::Pen>(st_col, 1.0f);
      else ac.stroke->SetColor(st_col);
      g.DrawPath(ac.stroke.get(), &clip);
    }

    // Text (with skip-direction slide)
    const float tx = static_cast<float>(std::lround((kPadLeft + kArtSize + kGap) * s));
    const float avail = W - tx - static_cast<float>(RightReservePx());

    if (slide_active) {
      const float te = static_cast<float>(EaseOutCubic(slide_t));
      const float off = (cfg.reduced_motion || !cfg.text_slide) ? 0.0f : static_cast<float>(kSlideOff * s * slide_dir);
      DrawTextBlock(g, tx, avail, H, -off * te, 1.0f - te, old_title, old_artist, old_tw, old_aw,
                    false, false, old_dim, nullptr, nullptr);
      DrawTextBlock(g, tx, avail, H, off * (1.0f - te), te, title_text, artist_text, title_w, artist_w,
                    title_over, artist_over, dim, &title_mq, &artist_mq);
    } else {
      DrawTextBlock(g, tx, avail, H, 0.0f, 1.0f, title_text, artist_text, title_w, artist_w,
                    title_over, artist_over, dim, &title_mq, &artist_mq);
    }

    DrawLyricLine(g, tx, avail, H);
    DrawNudge(g, tx, avail, H);
    DrawVolume(g, tx, avail, H);
    // The visualizer is NOT drawn here: it is the top layer, painted by DrawViz() after the static layer
    // has been snapshotted (see Render()).
  }

  // ---- layers ---------------------------------------------------------------
  // Everything except the visualizer is the "static layer". Render() snapshots it after each full draw, so a
  // visualizer-only frame (the common playing-with-no-other-animation case, ~30 fps) restores a small rect
  // from the snapshot, redraws just the visualizer into it and tells DWM only that rect changed.

  void SetupGraphics(gp::Graphics& g) const {
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    g.SetTextRenderingHint(kCapsuleTextHint);   // no ClearType on alpha
    g.SetTextContrast(kCapsuleTextContrast);
  }

  // Hover scale: one transform for the whole capsule group (1.0 = none).
  float RootScale() const {
    const float sc = 1.0f + static_cast<float>(kHoverGrow * hov_sp.x);
    return std::fabs(sc - 1.0f) > 0.0005f ? sc : 1.0f;
  }

  void ApplyRootTransform(gp::Graphics& g) const {
    const float sc = RootScale();
    if (sc == 1.0f) return;
    const float cx = static_cast<float>(win_w) * 0.5f;
    const float cy = static_cast<float>(pill_off_y) + static_cast<float>(pill_h) * 0.5f;   // scale about the capsule
    g.TranslateTransform(cx, cy);
    g.ScaleTransform(sc, sc);
    g.TranslateTransform(-cx, -cy);
  }

  // Visualizer slot: fixed on the right, inside the pill (pill-local coordinates).
  void VizOrigin(float& vx, float& vy) const {
    const double s = Scale();
    vx = static_cast<float>(pill_w) - static_cast<float>(std::lround(kPadRightViz * s)) -
         static_cast<float>(viz.SlotWidth());
    vy = (static_cast<float>(pill_h) - static_cast<float>(viz.SlotHeight())) * 0.5f;
  }

  // Window-pixel rect the visualizer is allowed to touch: its slot + a margin for the glow halo, carried
  // through the hover scale, clamped to the window. The visualizer is clipped to it in EVERY frame, so the
  // snapshot restore + dirty rect are exactly the pixels it can ever change.
  RECT VizRect() const {
    float vx, vy;
    VizOrigin(vx, vy);
    const float sw = static_cast<float>(viz.SlotWidth()), sh = static_cast<float>(viz.SlotHeight());
    const float m = std::ceil(static_cast<float>(kVizMargin) * (std::max)(sw, sh)) + 2.0f;
    float x0 = static_cast<float>(pill_off_x) + vx - m, y0 = static_cast<float>(pill_off_y) + vy - m;
    float x1 = x0 + sw + 2.0f * m, y1 = y0 + sh + 2.0f * m;
    const float sc = RootScale();
    if (sc != 1.0f) {
      const float cx = static_cast<float>(win_w) * 0.5f;
      const float cy = static_cast<float>(pill_off_y) + static_cast<float>(pill_h) * 0.5f;
      x0 = cx + (x0 - cx) * sc; x1 = cx + (x1 - cx) * sc;
      y0 = cy + (y0 - cy) * sc; y1 = cy + (y1 - cy) * sc;
    }
    RECT r{ static_cast<LONG>(std::floor(x0)), static_cast<LONG>(std::floor(y0)),
            static_cast<LONG>(std::ceil(x1)),  static_cast<LONG>(std::ceil(y1)) };
    r.left = (std::clamp)(r.left, 0L, static_cast<LONG>(win_w));
    r.right = (std::clamp)(r.right, 0L, static_cast<LONG>(win_w));
    r.top = (std::clamp)(r.top, 0L, static_cast<LONG>(win_h));
    r.bottom = (std::clamp)(r.bottom, 0L, static_cast<LONG>(win_h));
    return r;
  }

  void DrawViz(gp::Graphics& g, const RECT& clip) {
    if (!viz.Enabled() || clip.right <= clip.left || clip.bottom <= clip.top) return;
    float vx, vy;
    VizOrigin(vx, vy);
    SetupGraphics(g);
    g.ResetTransform();
    g.SetClip(gp::Rect(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top), gp::CombineModeReplace);
    ApplyRootTransform(g);
    g.TranslateTransform(static_cast<float>(pill_off_x), static_cast<float>(pill_off_y));
    viz.Draw(g, vx, vy, style.viz, style.viz_a);
    g.ResetTransform();
    g.ResetClip();
  }

  // Static layer: everything except the visualizer.
  void Draw(gp::Graphics& g) {
    SetupGraphics(g);
    g.Clear(gp::Color(0, 0, 0, 0));

    // Hover scale: one transform for the whole capsule group, so text/art/strip scale together
    // (text is re-rendered at the scaled size, not stretched as a bitmap).
    const gp::GraphicsState root = g.Save();
    ApplyRootTransform(g);

    // 1) transport strip, BEHIND the main pill: its inner end is hidden under the pill
    if (Expand() > 0.01 && extra_px > 0) {
      const float a = static_cast<float>((std::clamp)(Expand(), 0.0, 1.0));
      gp::GraphicsPath pill_shape;
      AddPill(pill_shape, static_cast<float>(pill_off_x), static_cast<float>(pill_off_y), static_cast<float>(pill_w),
              static_cast<float>(pill_h));

      const gp::GraphicsState st = g.Save();
      g.SetClip(&pill_shape, gp::CombineModeExclude);   // never blend glass over glass
      g.TranslateTransform(0.0f, static_cast<float>(pill_off_y));
      strip.Draw(g, static_cast<float>(strip_x), a, style, CurrentTransport());
      if (seam_active) {
        g.TranslateTransform(static_cast<float>(strip_x), 0.0f);
        DrawSeam(g, a);
      }
      g.Restore(st);

      // soft contact shadow where the main pill sits on the strip
      const gp::GraphicsState st2 = g.Save();
      g.SetClip(&pill_shape, gp::CombineModeExclude);
      for (int i = 3; i >= 1; --i) {
        gp::Pen sh(gp::Color(static_cast<BYTE>(10 + (4 - i) * 8 * a), 0, 0, 0), static_cast<gp::REAL>(i * 2));
        g.DrawPath(&sh, &pill_shape);
      }
      g.Restore(st2);
    }

    // 2) main capsule on top (no scaling: everything stays on whole pixels)
    {
      const gp::GraphicsState st = g.Save();
      g.TranslateTransform(static_cast<float>(pill_off_x), static_cast<float>(pill_off_y));
      DrawSurface(g, static_cast<float>(pill_w), static_cast<float>(pill_h));
      DrawPill(g);
      g.Restore(st);
    }

    // 3) lyrics mode: the buttons ride on the tab, clipped to it so they slide up out of the capsule
    if (ExpandUp() > 0.01 && up_extra_px > 0 && pill_w - pill_h > 8) {
      const float a = static_cast<float>((std::clamp)(ExpandUp(), 0.0, 1.0));
      const float tw = UpTabWidth();
      const float tx0 = static_cast<float>(pill_off_x) + (static_cast<float>(pill_w) - tw) * 0.5f;
      const gp::GraphicsState st = g.Save();
      g.SetClip(gp::RectF(tx0, 0.0f, tw, static_cast<float>(pill_off_y)), gp::CombineModeIntersect);
      strip.DrawUpButtons(g, tx0, static_cast<float>(pill_off_y - up_extra_px), tw, a, style, CurrentTransport());
      g.Restore(st);
    }

    g.Restore(root);
  }

  // ---- cached GDI+ objects --------------------------------------------------
  // Paths / brushes / pens / scratch bitmaps that used to be created and destroyed on every frame. Each is
  // rebuilt only when its geometry key changes; colours that animate are updated in place (SetColor ...).
  struct SurfaceCache {          // capsule glass: body + rim paths, gradient brush, rim pen
    float W = -1.0f, H = -1.0f, U = 0.0f, tw = 0.0f;
    bool merged = false;
    std::unique_ptr<gp::GraphicsPath> body, rim_path;
    std::unique_ptr<gp::LinearGradientBrush> fill;
    std::unique_ptr<gp::Pen> rim;
    void Release() {
      fill.reset(); rim.reset(); body.reset(); rim_path.reset();
      W = -1.0f;
    }
  } surf;

  struct ArtGfxCache {           // cover: rounded clip path, placeholder brush, outline pen
    int px = -1;
    float x = 0.0f, y = 0.0f, r = 0.0f;
    std::unique_ptr<gp::GraphicsPath> clip;
    std::unique_ptr<gp::SolidBrush> placeholder;
    std::unique_ptr<gp::Pen> stroke;
    void Release() {
      clip.reset(); placeholder.reset(); stroke.reset();
      px = -1;
    }
  } art_gfx;

  struct MarqueeScratch {        // off-screen strip a scrolling line is rendered into (title = 0, artist = 1)
    int w = 0, h = 0;
    std::unique_ptr<gp::Bitmap> bmp;   // only the bitmap is kept; the cheap Graphics is made per frame
    void Release() {
      bmp.reset();
      w = h = 0;
    }
  } mq_scratch[2];

  void ReleaseDrawCaches() {     // GDI+ objects must go before GdiplusShutdown
    surf.Release();
    art_gfx.Release();
    mq_scratch[0].Release();
    mq_scratch[1].Release();
    scaled_cur = ScaledArt{};
    scaled_prev = ScaledArt{};
    viz.ReleaseGdi();
  }

  // ---- render target ------------------------------------------------------
  // One DIB section + memory DC kept for the lifetime of the window (it used to be created and destroyed on
  // EVERY frame: GetDC, CreateCompatibleDC, CreateDIBSection, ... ~60x/s while animating). The DIB has spare
  // capacity, so the width animation does not reallocate it; only the cheap GDI+ wrapper is rebuilt when the
  // window size changes.
  HDC rt_dc = nullptr;
  HBITMAP rt_dib = nullptr;
  HGDIOBJ rt_old = nullptr;
  void* rt_bits = nullptr;
  int rt_cap_w = 0, rt_cap_h = 0;        // allocated DIB size (>= window size)
  int rt_w = 0, rt_h = 0;                // size the GDI+ wrapper currently covers
  std::unique_ptr<gp::Bitmap> rt_bmp;
  std::unique_ptr<gp::Graphics> rt_g;

  // Static-layer snapshot (everything but the visualizer), taken after each full draw.
  struct SnapKey {              // what the snapshot was drawn for: any difference => full redraw
    int win_w = -1, win_h = -1, off_x = 0, off_y = 0, pill_w = 0, pill_h = 0, dpi = 0;
    float hov = 0.0f, acc_r = 0.0f, acc_g = 0.0f, acc_b = 0.0f, str = 0.0f;
    bool operator==(const SnapKey&) const = default;
  };
  std::vector<uint32_t> snap;   // win_w * win_h premultiplied pixels
  bool snap_valid = false;
  SnapKey snap_key;
  bool static_dirty = true;     // something non-animated changed (mouse, wheel, lyrics, ...): next frame is a full one

  // Last UpdateLayeredWindow call: a dirty rect is only valid when position / size / alpha are unchanged.
  bool ul_valid = false;
  int ul_x = 0, ul_y = 0, ul_w = 0, ul_h = 0;
  BYTE ul_alpha = 0;

  SnapKey MakeKey() const {
    SnapKey k;
    k.win_w = win_w; k.win_h = win_h; k.off_x = pill_off_x; k.off_y = pill_off_y;
    k.pill_w = pill_w; k.pill_h = pill_h; k.dpi = dpi;
    k.hov = static_cast<float>(hov_sp.x);
    k.acc_r = acc_cur.r; k.acc_g = acc_cur.g; k.acc_b = acc_cur.b; k.str = str_cur;
    return k;
  }

  void ReleaseTarget() {   // GDI+ objects must go before GdiplusShutdown
    snap_valid = false;
    ul_valid = false;
    std::vector<uint32_t>().swap(snap);
    rt_g.reset();
    rt_bmp.reset();
    if (rt_dc && rt_old) SelectObject(rt_dc, rt_old);
    if (rt_dib) DeleteObject(rt_dib);
    if (rt_dc) DeleteDC(rt_dc);
    rt_dc = nullptr; rt_dib = nullptr; rt_old = nullptr; rt_bits = nullptr;
    rt_cap_w = rt_cap_h = rt_w = rt_h = 0;
  }

  bool EnsureTarget() {
    if (rt_g && rt_w == win_w && rt_h == win_h) return true;

    // (Re)allocate the DIB only when the window outgrew it, or it is wastefully large (> 2x in an axis).
    const bool too_small = win_w > rt_cap_w || win_h > rt_cap_h;
    const bool too_big = rt_cap_w > win_w * 2 + 128 || rt_cap_h > win_h * 2 + 64;
    if (!rt_dib || too_small || too_big) {
      ReleaseTarget();
      const int cw = ((win_w + 63) / 64) * 64 + 64;   // headroom for the capsule growing while it animates
      const int ch = ((win_h + 15) / 16) * 16 + 16;
      BITMAPINFO bmi{};
      bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bmi.bmiHeader.biWidth = cw;
      bmi.bmiHeader.biHeight = -ch;   // top-down
      bmi.bmiHeader.biPlanes = 1;
      bmi.bmiHeader.biBitCount = 32;
      bmi.bmiHeader.biCompression = BI_RGB;
      rt_dc = CreateCompatibleDC(nullptr);   // memory DC compatible with the screen
      if (rt_dc) rt_dib = CreateDIBSection(rt_dc, &bmi, DIB_RGB_COLORS, &rt_bits, nullptr, 0);
      if (!rt_dc || !rt_dib || !rt_bits) {
        ReleaseTarget();
        return false;
      }
      rt_old = SelectObject(rt_dc, rt_dib);
      rt_cap_w = cw;
      rt_cap_h = ch;
    }

    // Wrapper over the top-left win_w x win_h part of the DIB (stride = the full DIB row).
    rt_g.reset();
    rt_bmp.reset();
    rt_bmp = std::make_unique<gp::Bitmap>(win_w, win_h, rt_cap_w * 4, PixelFormat32bppPARGB,
                                          static_cast<BYTE*>(rt_bits));
    rt_g = std::make_unique<gp::Graphics>(rt_bmp.get());
    rt_w = win_w;
    rt_h = win_h;
    return true;
  }

  // viz_only: the caller knows nothing but the visualizer changed since the last frame. Render() still
  // verifies that against the snapshot key and falls back to a full draw if anything differs.
  void Render(bool viz_only = false) {
    if (!hwnd || win_w <= 0 || win_h <= 0 || !fonts) return;
    if (!EnsureTarget()) return;

    const SnapKey key = MakeKey();
    const RECT vr = VizRect();
    const bool partial = viz_only && viz.Enabled() && snap_valid && !static_dirty && key == snap_key &&
                         vr.right > vr.left && vr.bottom > vr.top;

    BYTE* const base = static_cast<BYTE*>(rt_bits);
    const size_t stride = static_cast<size_t>(rt_cap_w) * 4;

    if (partial) {
      // Put the static pixels back under the visualizer, then redraw only the visualizer, clipped to that rect.
      const size_t bytes = static_cast<size_t>(vr.right - vr.left) * 4;
      for (int y = vr.top; y < vr.bottom; ++y) {
        std::memcpy(base + static_cast<size_t>(y) * stride + static_cast<size_t>(vr.left) * 4,
                    snap.data() + static_cast<size_t>(y) * win_w + vr.left, bytes);
      }
      DrawViz(*rt_g, vr);
    } else {
      rt_g->ResetTransform();
      rt_g->ResetClip();
      Draw(*rt_g);   // starts with g.Clear(transparent) over the whole window: no separate memset needed
      GdiFlush();

      snap.resize(static_cast<size_t>(win_w) * win_h);
      for (int y = 0; y < win_h; ++y)
        std::memcpy(snap.data() + static_cast<size_t>(y) * win_w, base + static_cast<size_t>(y) * stride,
                    static_cast<size_t>(win_w) * 4);
      snap_key = key;
      snap_valid = true;
      static_dirty = false;

      DrawViz(*rt_g, vr);
    }
    GdiFlush();

    POINT dst{ win_x, win_y };
    POINT src{ 0, 0 };
    SIZE size{ win_w, win_h };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = static_cast<BYTE>((std::clamp)(vis, 0.0, 1.0) * 255.0 + 0.5);
    blend.AlphaFormat = AC_SRC_ALPHA;

    // Same position, size and alpha as the previous update => DWM only needs to take the visualizer rect.
    const bool use_dirty = partial && ul_valid && ul_x == win_x && ul_y == win_y && ul_w == win_w &&
                           ul_h == win_h && ul_alpha == blend.SourceConstantAlpha;
    RECT dirty = vr;
    UPDATELAYEREDWINDOWINFO info{};
    info.cbSize = sizeof(info);
    info.pptDst = &dst;
    info.psize = &size;
    info.hdcSrc = rt_dc;
    info.pptSrc = &src;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = use_dirty ? &dirty : nullptr;
    ul_valid = UpdateLayeredWindowIndirect(hwnd, &info) != FALSE;
    ul_x = win_x; ul_y = win_y; ul_w = win_w; ul_h = win_h; ul_alpha = blend.SourceConstantAlpha;
  }

  // ---- placement ----------------------------------------------------------

  int AnchorX(HWND taskbar, const RECT& rc, int w) const {
    const int margin = static_cast<int>(std::lround(cfg.margin * Scale()));
    switch (cfg.anchor) {
      case PillAnchor::Left:
        return rc.left + margin;
      case PillAnchor::Right: {
        int right_limit = rc.right;
        RECT tray{};
        HWND tn = FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr);
        if (tn && GetWindowRect(tn, &tray) && tray.left > rc.left) right_limit = tray.left;
        return right_limit - margin - w;
      }
      case PillAnchor::Center:
      default:
        return rc.left + ((rc.right - rc.left) - w) / 2;
    }
  }

  void Tick() {
    if (!hwnd) return;
    SyncWheelHook();
    SyncKeyHook();

    const double dt = FrameDt();
    since_render += dt;
    since_zorder += dt;
    env_since += dt;
    if (z_burst > 0.0) {   // just switched apps: put the capsule back above the taskbar right away
      z_burst -= dt;
      if (since_zorder < 0.5) since_zorder = 0.5;
    }

    // The taskbar window is looked up once; only a dead handle (explorer restart) triggers a new search.
    if (!tb_cached || !IsWindow(tb_cached)) {
      tb_cached = FindWindowW(L"Shell_TrayWnd", nullptr);
      env_dirty = true;
    }
    HWND tb = tb_cached;
    RECT rc{};
    if (!tb || !GetWindowRect(tb, &rc)) return;   // explorer restarting: keep last frame

    {   // cursor over the taskbar or the thumbnail / jump-list area right above it => fast z-order guard
      POINT cp{};
      RECT hot = rc;
      hot.top -= static_cast<LONG>(std::lround(kHotAboveDp * Scale()));
      if (GetCursorPos(&cp) && PtInRect(&hot, cp)) hot_hold = kHotHold;
      else if (hot_hold > 0.0) hot_hold -= dt;
    }

    if (env_dirty || env_since >= kEnvPoll) {
      UINT dx = 96, dy = 96;
      if (HMONITOR mon = MonitorFromWindow(tb, MONITOR_DEFAULTTONEAREST))
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
      env_dpi = static_cast<int>(dx);
      env_dirty = false;
      env_since = 0.0;
    }

    const bool dpi_changed = env_dpi != dpi;
    const bool need_layout = content_dirty || !laid_out || dpi_changed;
    dpi = env_dpi;

    if (need_layout) {
      Layout(!laid_out || dpi_changed);
      laid_out = true;
      content_dirty = false;
    }

    // no media: hide only after the session has been gone for a moment (track gaps)
    if (media->has_session) idle_for = 0.0;
    else idle_for += dt;
    if (!cfg.hide_fullscreen) {   // setting turned off: always show
      fullscreen = false;
    } else {
      // A point on the taskbar just outside the capsule's window (clears its transparent padding):
      // only used to notice an auto-hidden taskbar that slid off screen.
      POINT probe{};
      probe.y = rc.top + (rc.bottom - rc.top) / 2;
      if (win_w > 0) {
        const int gap = static_cast<int>(std::lround(14.0 * Scale()));
        probe.x = (win_x - gap > rc.left + 2) ? win_x - gap : (std::min)(win_x + win_w + gap, static_cast<int>(rc.right) - 2);
      } else {
        probe.x = rc.left + (rc.right - rc.left) / 2;
      }
      wchar_t who[96] = {};
      const Cover c = TaskbarCover(hwnd, tb, probe, who, 96);   // a handful of cheap calls, every tick
      if (c != Cover::Unknown && (c == Cover::Yes) != fullscreen) {
        // No debounce: the foreground-window test has no sampling noise, and hiding / showing right
        // away is what keeps the capsule in step with the taskbar.
        const bool covered = c == Cover::Yes;
        Logf(std::wstring(L"[taskbar] ") + (covered ? L"covered by " : L"visible again") + (covered ? who : L"") +
             L"\r\n");
        if (covered) vis = 0.0;                    // hide instantly, no fade-out
        else vis = (std::max)(vis, 0.45);          // and come back quickly (the rest fades in)
        fullscreen = covered;
      }
    }
    // no media session (nothing playing / filtered out) => the capsule is simply hidden
    const bool want_hidden = (!media->has_session && idle_for > kIdleHide) || fullscreen;

    const bool lyric_changed = UpdateLyrics();
    const bool major = Animate(dt, want_hidden) || lyric_changed;
    // Audio capture only while it can be seen: visualizer on, the setting on, a session playing, capsule visible.
    audio.SetActive(cfg.visualizer_audio && viz.Enabled() && media->has_session && media->is_playing && !want_hidden);
    const bool viz_changed = viz.Update(dt);
    if (viz_changed) pending_viz = true;
    if (!style_valid || style_str != str_cur || style_acc.r != acc_cur.r ||
        style_acc.g != acc_cur.g || style_acc.b != acc_cur.b) {
      style = ComposeGlass(dark, acc_cur, str_cur);   // pow()s + colour mixes: only when the colour moved
      style_valid = true;
      style_str = str_cur;
      style_acc = acc_cur;
    }

    major_anim = major;
    viz_anim = viz_changed;

    if (want_hidden && vis <= 0.004) {
      if (shown) {
        ShowWindow(hwnd, SW_HIDE);
        shown = false;
        ul_valid = false;
        snap_valid = false;
      }
      hidden = true;
      major_anim = viz_anim = pending_viz = false;
      return;
    }
    hidden = false;

    // geometry for this frame
    const double s = Scale();
    pad_px = static_cast<int>(std::lround(kWinPad * s));
    const double ex = (std::clamp)(Expand(), 0.0, 1.08);
    // visible part of the strip beyond the pill edge (the rest stays tucked under the pill)
    const int extra = static_cast<int>(std::lround((strip.Width() - strip.Overlap()) * ex));
    const int dir = Dir();
    // main capsule width: autofit width, shrunk toward the visualizer-only width while paused
    const double col = (std::clamp)(col_sp.x, 0.0, 1.0);
    pill_w = static_cast<int>(std::lround(cur_w + (CollapsedW() - cur_w) * col));
    extra_px = extra;
    // lyrics mode: the transport panel above the capsule makes the window taller (the capsule keeps its place)
    const double exu = (std::clamp)(ExpandUp(), 0.0, 1.08);
    up_extra_px = static_cast<int>(std::lround(strip.UpHeight() * exu));
    win_w = pill_w + extra + 2 * pad_px;
    win_h = pill_h + 2 * pad_px + up_extra_px;
    pill_off_y = pad_px + up_extra_px;

    // The whole group (strip + main capsule) is anchored/centered together, so when the strip
    // opens the main capsule slides to the opposite side and the pair stays centered.
    const int group_x = AnchorX(tb, rc, pill_w + extra);
    const int nx = group_x - pad_px;
    const int ny = rc.top + ((rc.bottom - rc.top) - pill_h) / 2 - pad_px - up_extra_px;   // capsule stays centered on the taskbar

    pill_off_x = dir > 0 ? pad_px : pad_px + extra;
    // The strip rides on the revealed edge, so it slides out from behind the pill.
    strip_x = dir > 0 ? pill_off_x + pill_w + extra - strip.Width() : pill_off_x - extra;
    clip_x = dir > 0 ? pill_off_x + pill_w : pill_off_x - extra;
    clip_w = extra;

    const bool do_render = need_layout || major || !shown ||
                           (pending_viz && since_render >= kVizFrame);
    if (do_render) {
      win_x = nx;
      win_y = ny;
      Render(!need_layout && !major && shown);   // nothing but the visualizer moved => partial redraw
      since_render = 0.0;
      pending_viz = false;
      if (!shown) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        shown = true;
        since_zorder = 0.0;
        return;
      }
    } else if (nx != win_x || ny != win_y) {
      win_x = nx;
      win_y = ny;
      SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
      return;
    }

    // Keep above the taskbar if it re-asserted its own z-order (cheap; every frame during the burst).
    if (since_zorder >= 0.5 || TaskbarAbove(hwnd, tb)) {
      since_zorder = 0.0;
      SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
  }

  // ---- mouse --------------------------------------------------------------

  int HitButton(int mx, int my) const {
    if (ExpandUp() >= 0.6) {   // the panel above the capsule (lyrics mode)
      if (my >= pill_off_y) return -1;
      const int tx0 = pill_off_x + static_cast<int>(std::lround((pill_w - UpTabWidth()) * 0.5f));
      return strip.HitTestUp(mx - tx0, my - (pill_off_y - up_extra_px), static_cast<int>(std::lround(UpTabWidth())));
    }
    if (Expand() < 0.6) return -1;
    return strip.HitTest(mx - strip_x, my - pill_off_y);
  }

  bool ButtonEnabled(int b) const {
    const MediaInfo& m = *media;
    if (b == TransportStrip::kPrev) return m.can_prev;
    if (b == TransportStrip::kPlayPause) return m.can_play_pause;
    if (b == TransportStrip::kNext) return m.can_next;
    return false;
  }

  void OnMouse(UINT msg, LPARAM lp) {
    const int mx = static_cast<int>(static_cast<short>(LOWORD(lp)));
    const int my = static_cast<int>(static_cast<short>(HIWORD(lp)));

    switch (msg) {
      case WM_MOUSEMOVE: {
        hover = true;
        hold = kHoverHold;
        if (!tracking) {
          TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, hwnd, 0 };
          if (TrackMouseEvent(&t)) tracking = true;
        }
        strip.SetHot(HitButton(mx, my));
        break;
      }
      case WM_MOUSELEAVE:
        tracking = false;
        hover = false;
        hold = kHoverHold;
        if (pressed < 0) strip.SetHot(-1);
        break;
      case WM_LBUTTONDOWN: {
        const int b = HitButton(mx, my);
        if (b >= 0 && ButtonEnabled(b)) {
          pressed = b;
          strip.SetPressed(b);
          strip.SetHot(b);
          SetCapture(hwnd);
        }
        break;
      }
      case WM_LBUTTONUP: {
        if (pressed >= 0) {
          const int b = HitButton(mx, my);
          const int was = pressed;
          pressed = -1;
          strip.SetPressed(-1);
          ReleaseCapture();
          strip.SetHot(b);
          if (b == was && on_command) {
            if (was == TransportStrip::kPrev)           { NotifySkip(-1); on_command(PillCommand::Previous); }
            else if (was == TransportStrip::kPlayPause) on_command(PillCommand::PlayPause);
            else if (was == TransportStrip::kNext)      { NotifySkip(+1); on_command(PillCommand::Next); }
          }
        }
        break;
      }
      case WM_CAPTURECHANGED:
        if (pressed >= 0) {
          pressed = -1;
          strip.SetPressed(-1);
        }
        break;
      // Click actions: only a press that started on the capsule counts (so a release after a drag from elsewhere
      // never fires).
      case WM_MBUTTONDOWN:
        mid_down = cfg.click_actions;
        break;
      case WM_MBUTTONUP:
        if (mid_down) {
          mid_down = false;
          if (cfg.click_actions && media->has_session && media->can_play_pause && on_command)
            on_command(PillCommand::PlayPause);
        }
        break;
      case WM_RBUTTONDOWN:
        right_down = cfg.click_actions;
        break;
      case WM_RBUTTONUP:
        if (right_down) {
          right_down = false;
          if (cfg.click_actions) ShowContextMenu();
        }
        break;
      default:
        break;
    }
    SyncKeyHook();       // Ctrl+L capture follows hover exactly (installed on enter, removed on leave)
    static_dirty = true;
    major_anim = true;   // make sure the next frames run at full pace
  }

  // ---- click actions -------------------------------------------------------------------------
  bool mid_down = false, right_down = false;

  bool CopyText(const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    bool ok = false;
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
      if (void* p = GlobalLock(h)) {
        std::memcpy(p, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;   // the clipboard owns the memory on success
      }
      if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
  }

  // Right-click menu – custom-drawn to match the settings window (same palette, fonts, rounding).
  // Modal: runs its own message loop until the user picks an item or clicks away.
  // Right-click menu – custom-drawn to match the settings window (same palette, fonts, rounding).
  // Layered popup + UpdateLayeredWindow so corners are clean (no region / no GDI ghosting).
  // Modal: own message loop until the user picks an item or clicks away.
  bool menu_open = false;          // a context menu is already showing (its loop is running)
  HWND menu_hwnd_active = nullptr; // so a second right-click can dismiss it instead of stacking another

  void ShowContextMenu() {
    // Never stack menus: the capsule is WS_EX_NOACTIVATE, so right-clicking it while the menu is open does not
    // deactivate the menu. Without this guard every click re-entered this function (nested loops, ghost menus).
    if (menu_open) {
      if (menu_hwnd_active && IsWindow(menu_hwnd_active)) DestroyWindow(menu_hwnd_active);
      return;
    }
    menu_open = true;
    struct OpenGuard {
      Impl* self;
      ~OpenGuard() { self->menu_open = false; self->menu_hwnd_active = nullptr; }
    } open_guard{ this };

    enum : int { kCopy = 1, kLyrics, kResetTiming, kSettings, kQuit };

    struct Item {
      int id = 0;
      const wchar_t* label = nullptr;
      const wchar_t* shortcut = nullptr;
      bool enabled = true;
      bool checked = false;
      bool sep = false;
    };

    const bool have_track = media->has_session && !media->title.empty();
    Item items[] = {
      { kCopy,        L"Copy title and artist", nullptr,      have_track,         false, false },
      { kLyrics,      L"Lyrics mode",           L"Ctrl+L",    cfg.lyrics_enabled, lyrics_user_on, false },
      { kResetTiming, L"Reset lyrics timing",   L"Ctrl+Down", lyric_ok,           false, false },
      { 0, nullptr, nullptr, false, false, true },
      { kSettings,    L"Open settings",         nullptr,      true,               false, false },
      { kQuit,        L"Quit",                  nullptr,      true,               false, false },
    };
    constexpr int kCount = static_cast<int>(sizeof(items) / sizeof(items[0]));

    // Same colours as the settings window (AppsUseLightTheme).
    DWORD light = 1, lsz = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &lsz);
    const bool dark = (light == 0);
    auto rgb = [](BYTE a, int r, int g, int b) {
      return gp::Color(a, static_cast<BYTE>(r), static_cast<BYTE>(g), static_cast<BYTE>(b));
    };
    const gp::Color col_bg     = dark ? rgb(255, 43, 43, 43)   : rgb(255, 255, 255, 255);
    const gp::Color col_border = dark ? rgb(255, 60, 60, 60)   : rgb(255, 226, 226, 228);
    const gp::Color col_text   = dark ? rgb(255, 255, 255, 255) : rgb(255, 27, 27, 27);
    const gp::Color col_sub    = dark ? rgb(255, 160, 160, 164) : rgb(255, 98, 98, 104);
    const gp::Color col_muted  = dark ? rgb(255, 104, 104, 108) : rgb(255, 176, 176, 182);
    const gp::Color col_hover  = dark ? rgb(255, 58, 58, 60)    : rgb(255, 226, 226, 230);
    const gp::Color col_accent = dark ? rgb(255, 104, 128, 255) : rgb(255, 80, 104, 240);
    const gp::Color col_div    = dark ? rgb(255, 56, 56, 56)    : rgb(255, 238, 238, 240);

    const float dpi = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
    // Whole-pixel metrics: fractional text origins / font sizes are what made the text look soft.
    auto px = [&](float v) { return std::round(v * dpi); };
    const float pad_x = px(14.0f);
    const float pad_y = px(6.0f);
    const float row_h = 2.0f * std::round(16.0f * dpi);   // even, so the text's vertical centre is a whole pixel
    const float sep_h = px(9.0f);
    const float radius = px(8.0f);
    const float check_w = px(20.0f);
    const float shortcut_gap = px(28.0f);

    gp::FontFamily fam(L"Segoe UI");
    gp::Font font_label(&fam, px(12.5f), gp::FontStyleRegular, gp::UnitPixel);
    gp::Font font_sc(&fam, px(11.5f), gp::FontStyleRegular, gp::UnitPixel);

    float max_label = 0.0f, max_sc = 0.0f;
    {
      HDC screen = GetDC(nullptr);
      gp::Graphics measure(screen);
      measure.SetTextRenderingHint(gp::TextRenderingHintAntiAliasGridFit);
      gp::StringFormat fmt(gp::StringFormat::GenericTypographic());
      fmt.SetFormatFlags(gp::StringFormatFlagsNoWrap);
      for (int i = 0; i < kCount; ++i) {
        if (items[i].sep || !items[i].label) continue;
        gp::RectF b;
        measure.MeasureString(items[i].label, -1, &font_label, gp::PointF(0, 0), &fmt, &b);
        if (b.Width > max_label) max_label = b.Width;
        if (items[i].shortcut) {
          measure.MeasureString(items[i].shortcut, -1, &font_sc, gp::PointF(0, 0), &fmt, &b);
          if (b.Width > max_sc) max_sc = b.Width;
        }
      }
      ReleaseDC(nullptr, screen);
    }

    const int win_w = static_cast<int>(std::ceil(pad_x + check_w + max_label + (max_sc > 0 ? shortcut_gap + max_sc : 0) + pad_x));
    float content_h = pad_y * 2.0f;
    for (int i = 0; i < kCount; ++i) content_h += items[i].sep ? sep_h : row_h;
    const int win_h = static_cast<int>(content_h + 0.5f);

    POINT cursor{};
    GetCursorPos(&cursor);
    int wx = cursor.x - win_w / 2;
    int wy = cursor.y - win_h - static_cast<int>(6 * dpi);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi)) {
      if (wx < mi.rcWork.left) wx = mi.rcWork.left + 4;
      if (wx + win_w > mi.rcWork.right) wx = mi.rcWork.right - win_w - 4;
      if (wy < mi.rcWork.top) wy = cursor.y + static_cast<int>(6 * dpi);
      if (wy + win_h > mi.rcWork.bottom) wy = mi.rcWork.bottom - win_h - 4;
    }

    struct MenuState {
      Item* items = nullptr;
      int count = 0;
      int hover = -1;
      int chosen = 0;
      bool done = false;
      float dpi = 1, pad_x = 0, pad_y = 0, row_h = 0, sep_h = 0, radius = 0, check_w = 0;
      int win_w = 0, win_h = 0;
      gp::Color bg, border, text, sub, muted, hover_bg, accent, div;
      gp::Font* font_label = nullptr;
      gp::Font* font_sc = nullptr;
      HWND hwnd = nullptr;
      HINSTANCE hinst = nullptr;

      void PaintLayered() {
        if (!hwnd) return;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = win_w;
        bi.bmiHeader.biHeight = -win_h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) {
          if (dib) DeleteObject(dib);
          DeleteDC(mem);
          ReleaseDC(nullptr, screen);
          return;
        }
        HGDIOBJ old = SelectObject(mem, dib);
        memset(bits, 0, static_cast<size_t>(win_w) * static_cast<size_t>(win_h) * 4);

        {
          gp::Graphics g(mem);
          g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
          g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
          g.Clear(gp::Color(0, 0, 0, 0));   // fully transparent outside the card

          // Card fill
          {
            gp::GraphicsPath path;
            AddRoundRect(path, 0.5f, 0.5f, static_cast<float>(win_w) - 1.0f, static_cast<float>(win_h) - 1.0f, radius);
            gp::SolidBrush fill(bg);
            g.FillPath(&fill, &path);
            gp::Pen pen(border, 1.0f);
            g.DrawPath(&pen, &path);
            // Clip subsequent drawing to the card so nothing bleeds outside the round rect.
            g.SetClip(&path);
          }
          // The card is fully opaque from here on, so ClearType is safe (the old ghosting came from text drawn
          // over transparent pixels). Text goes after the card fill and uses the same hint as the measuring pass.
          g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);

          gp::StringFormat near_fmt(gp::StringFormat::GenericTypographic());
          near_fmt.SetAlignment(gp::StringAlignmentNear);
          near_fmt.SetLineAlignment(gp::StringAlignmentCenter);
          near_fmt.SetFormatFlags(gp::StringFormatFlagsNoWrap);
          gp::StringFormat far_fmt(gp::StringFormat::GenericTypographic());
          far_fmt.SetAlignment(gp::StringAlignmentFar);
          far_fmt.SetLineAlignment(gp::StringAlignmentCenter);
          far_fmt.SetFormatFlags(gp::StringFormatFlagsNoWrap);

          float y = pad_y;
          for (int i = 0; i < count; ++i) {
            const Item& it = items[i];
            if (it.sep) {
              const float mid = y + sep_h * 0.5f;
              gp::Pen pen(div, 1.0f);
              g.DrawLine(&pen, pad_x, mid, static_cast<float>(win_w) - pad_x, mid);
              y += sep_h;
              continue;
            }
            if (i == hover && it.enabled) {
              gp::GraphicsPath hp;
              AddRoundRect(hp, 4.0f * dpi, y + 1.0f * dpi,
                           static_cast<float>(win_w) - 8.0f * dpi, row_h - 2.0f * dpi, 6.0f * dpi);
              gp::SolidBrush hb(hover_bg);
              g.FillPath(&hb, &hp);
            }
            const gp::Color& tc = it.enabled ? text : muted;
            if (it.checked) {
              const float cx = pad_x + check_w * 0.40f;
              const float cy = y + row_h * 0.5f;
              gp::Pen tick(accent, 1.7f * dpi);
              tick.SetLineCap(gp::LineCapRound, gp::LineCapRound, gp::DashCapRound);
              g.DrawLine(&tick, cx - 3.0f * dpi, cy, cx - 0.5f * dpi, cy + 3.0f * dpi);
              g.DrawLine(&tick, cx - 0.5f * dpi, cy + 3.0f * dpi, cx + 4.5f * dpi, cy - 3.5f * dpi);
            }
            gp::SolidBrush tb(tc);
            g.DrawString(it.label, -1, font_label,
                         gp::RectF(pad_x + check_w, y, static_cast<float>(win_w) - pad_x - check_w - pad_x, row_h),
                         &near_fmt, &tb);
            if (it.shortcut) {
              gp::SolidBrush sb(sub);
              g.DrawString(it.shortcut, -1, font_sc,
                           gp::RectF(pad_x, y, static_cast<float>(win_w) - 2.0f * pad_x, row_h),
                           &far_fmt, &sb);
            }
            y += row_h;
          }
        }

        // Premultiply for UpdateLayeredWindow
        auto* px = static_cast<BYTE*>(bits);
        const size_t n = static_cast<size_t>(win_w) * static_cast<size_t>(win_h);
        for (size_t i = 0; i < n; ++i) {
          BYTE* p = px + i * 4;
          const unsigned a = p[3];
          if (a == 0) { p[0] = p[1] = p[2] = 0; }
          else if (a < 255) {
            p[0] = static_cast<BYTE>(p[0] * a / 255);
            p[1] = static_cast<BYTE>(p[1] * a / 255);
            p[2] = static_cast<BYTE>(p[2] * a / 255);
          }
        }

        RECT wr{};
        GetWindowRect(hwnd, &wr);
        POINT pt_dst{ wr.left, wr.top };
        SIZE size{ win_w, win_h };
        POINT pt_src{ 0, 0 };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(hwnd, screen, &pt_dst, &size, mem, &pt_src, 0, &blend, ULW_ALPHA);

        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
      }

      int HitRow(float my) const {
        float top = pad_y;
        for (int i = 0; i < count; ++i) {
          const float h = items[i].sep ? sep_h : row_h;
          if (!items[i].sep && my >= top && my < top + h) return i;
          top += h;
        }
        return -1;
      }
    } st;

    st.items = items; st.count = kCount;
    st.dpi = dpi; st.pad_x = pad_x; st.pad_y = pad_y; st.row_h = row_h; st.sep_h = sep_h;
    st.radius = radius; st.check_w = check_w; st.win_w = win_w; st.win_h = win_h;
    st.bg = col_bg; st.border = col_border; st.text = col_text; st.sub = col_sub;
    st.muted = col_muted; st.hover_bg = col_hover; st.accent = col_accent; st.div = col_div;
    st.font_label = &font_label; st.font_sc = &font_sc; st.hinst = hinst;

    static constexpr wchar_t kMenuClass[] = L"IslandCapsuleCtxMenu";
    static bool class_reg = false;
    if (!class_reg) {
      WNDCLASSEXW wc{ sizeof(wc) };
      wc.lpfnWndProc = [](HWND hw, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        auto* s = reinterpret_cast<MenuState*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
        switch (msg) {
          case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
          }
          case WM_MOUSEMOVE: {
            if (!s) break;
            const float my = static_cast<float>(static_cast<short>(HIWORD(lp)));
            const int hit = s->HitRow(my);
            if (hit != s->hover) {
              s->hover = hit;
              s->PaintLayered();
            }
            return 0;
          }
          case WM_LBUTTONDOWN:
          case WM_RBUTTONDOWN:
          case WM_MBUTTONDOWN: {
            // The menu holds mouse capture, so a click anywhere else (including on the capsule) lands here
            // with coordinates outside the client area: dismiss and swallow it.
            if (!s) break;
            const int mx = static_cast<int>(static_cast<short>(LOWORD(lp)));
            const int my = static_cast<int>(static_cast<short>(HIWORD(lp)));
            if (mx < 0 || my < 0 || mx >= s->win_w || my >= s->win_h) {
              s->done = true;
              s->chosen = 0;
              DestroyWindow(hw);
            }
            return 0;
          }
          case WM_CAPTURECHANGED:
            if (s && !s->done) {   // something stole capture (another app popup): close
              s->done = true;
              s->chosen = 0;
              DestroyWindow(hw);
            }
            return 0;
          case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
          case WM_LBUTTONUP: {
            if (!s) break;
            const int mx = static_cast<int>(static_cast<short>(LOWORD(lp)));
            const float my = static_cast<float>(static_cast<short>(HIWORD(lp)));
            if (mx < 0 || mx >= s->win_w) return 0;
            const int hit = s->HitRow(my);
            if (hit >= 0 && s->items[hit].enabled) {
              s->chosen = s->items[hit].id;
              s->done = true;
              DestroyWindow(hw);
            }
            return 0;
          }
          case WM_RBUTTONUP:
          case WM_MBUTTONUP:
            return 0;
          case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
              if (s) { s->done = true; s->chosen = 0; }
              DestroyWindow(hw);
              return 0;
            }
            break;
          case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && s && !s->done) {
              s->done = true;
              s->chosen = 0;
              DestroyWindow(hw);
            }
            return 0;
          case WM_DESTROY:
            if (s) s->done = true;
            return 0;
        }
        return DefWindowProcW(hw, msg, wp, lp);
      };
      wc.hInstance = hinst;
      wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      wc.lpszClassName = kMenuClass;
      RegisterClassExW(&wc);
      class_reg = true;
    }

    HWND menu_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kMenuClass, L"",
        WS_POPUP,
        wx, wy, win_w, win_h,
        nullptr, nullptr, hinst, &st);   // no owner – avoids layered-parent composition glitches
    if (!menu_hwnd) return;
    st.hwnd = menu_hwnd;

    menu_hwnd_active = menu_hwnd;
    st.PaintLayered();
    ShowWindow(menu_hwnd, SW_SHOWNA);
    SetForegroundWindow(menu_hwnd);
    SetCapture(menu_hwnd);   // reliable outside-click dismissal even when foreground activation is refused

    // Modal loop, but it keeps the capsule alive: the main loop is blocked while we are in here, so Tick()
    // (z-order guard, taskbar-cover check, animation, media state) must be driven from this loop too.
    // Without it the capsule froze, fell behind the taskbar and vanished.
    MSG msg{};
    while (!st.done) {
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
          st.done = true;
          st.chosen = 0;
          PostQuitMessage(static_cast<int>(msg.wParam));
          break;
        }
        // Keys go to the menu even though it may not be the active window.
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (!st.done) Tick();
    }
    if (GetCapture() == menu_hwnd) ReleaseCapture();
    if (IsWindow(menu_hwnd)) DestroyWindow(menu_hwnd);

    switch (st.chosen) {
      case kCopy: {
        const std::wstring text = media->artist.empty() ? media->title : media->artist + L" - " + media->title;
        Logf(CopyText(text) ? L"[menu] copied title and artist\r\n" : L"[menu] could not open the clipboard\r\n");
        break;
      }
      case kLyrics:      ToggleLyrics(); break;
      case kResetTiming: NudgeLyrics(0); break;
      case kSettings:    OpenSettingsWindow(hinst); break;
      case kQuit:        PostQuitMessage(0); break;
      default: break;
    }
  }

  // ---- wheel capture -------------------------------------------------------------------------
  // WM_MOUSEWHEEL only reaches this no-activate window if Windows' "scroll inactive windows"
  // routing happens to apply, so a low-level mouse hook catches the wheel while the cursor is over
  // the capsule (and swallows it so the app underneath does not scroll as well).
  static constexpr UINT kMsgWheel = WM_APP + 21;
  inline static Impl* hook_owner = nullptr;
  HHOOK wheel_hook = nullptr;

  // ---- Ctrl+L (lyrics mode) ---------------------------------------------------------------------
  // A global hotkey would break Ctrl+L in browsers, so a low-level keyboard hook is installed ONLY while
  // the cursor is over the capsule (hover) and removed as soon as it leaves. While installed it swallows
  // Ctrl+L (and the matching key-up) and posts a toggle to the window; every other key passes straight
  // through. Same pattern as the wheel hook.
  static constexpr UINT kMsgToggleLyrics = WM_APP + 22;
  static constexpr UINT kMsgNudgeLyrics = WM_APP + 23;   // lParam = step in ms (0 = reset this song's nudge)
  inline static Impl* key_owner = nullptr;
  HHOOK key_hook = nullptr;
  bool l_swallowed = false;   // we ate the key-down of an L: also eat its auto-repeats and key-up
  DWORD nav_swallowed = 0;    // vk of the Ctrl+Left / Right / Down we ate (its key-up is eaten too; repeats nudge again)

  bool KeyHit(const POINT& pt) const {
    if (!shown || hidden || vis < 0.5 || !hover) return false;
    const RECT r{ win_x, win_y, win_x + win_w, win_y + win_h };
    return PtInRect(&r, pt) != FALSE;   // hover already means "over an opaque part of the capsule"
  }

  static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

  static LRESULT CALLBACK LowLevelKeyboard(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && key_owner && key_owner->hwnd) {
      const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
      if (kb->vkCode == VK_LEFT || kb->vkCode == VK_RIGHT || kb->vkCode == VK_DOWN) {
        // Lyrics timing nudge. Only while lyrics are actually shown and the cursor is over the capsule, and
        // only with Ctrl alone: everywhere else Ctrl+arrows (word jumps in editors, ...) pass straight through.
        Impl* self = key_owner;
        const bool is_down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        const bool is_up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
        if (is_down) {
          POINT pt{};
          if (self->lyric_ok && Down(VK_CONTROL) && !Down(VK_MENU) && !Down(VK_SHIFT) && !Down(VK_LWIN) &&
              !Down(VK_RWIN) && GetCursorPos(&pt) && self->KeyHit(pt)) {
            self->nav_swallowed = kb->vkCode;
            const int step = kb->vkCode == VK_LEFT ? -kNudgeStepMs : kb->vkCode == VK_RIGHT ? kNudgeStepMs : 0;
            PostMessageW(self->hwnd, kMsgNudgeLyrics, 0, static_cast<LPARAM>(step));
            return 1;
          }
        } else if (is_up && self->nav_swallowed == kb->vkCode) {
          self->nav_swallowed = 0;
          return 1;
        }
      } else if (kb->vkCode == 'L') {
        Impl* self = key_owner;
        const bool is_down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        const bool is_up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
        if (is_down) {
          if (self->l_swallowed) return 1;   // auto-repeat of the swallowed key
          POINT pt{};
          if (Down(VK_CONTROL) && !Down(VK_MENU) && !Down(VK_SHIFT) && !Down(VK_LWIN) && !Down(VK_RWIN) &&
              GetCursorPos(&pt) && self->KeyHit(pt)) {
            self->l_swallowed = true;
            PostMessageW(self->hwnd, kMsgToggleLyrics, 0, 0);
            return 1;   // the page / app under the capsule never sees this Ctrl+L
          }
        } else if (is_up && self->l_swallowed) {
          self->l_swallowed = false;
          return 1;
        }
      }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
  }

  void SyncKeyHook() {
    const bool want = cfg.lyrics_enabled && hwnd != nullptr && shown && hover;
    if (want && !key_hook) {
      key_owner = this;
      l_swallowed = false;
      nav_swallowed = 0;
      key_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboard, GetModuleHandleW(nullptr), 0);
      if (!key_hook) Logf(L"[lyrics] keyboard hook failed, error " + std::to_wstring(GetLastError()) + L"\r\n");
    } else if (!want && key_hook) {
      UnhookWindowsHookEx(key_hook);
      key_hook = nullptr;
      l_swallowed = false;
      nav_swallowed = 0;
      if (key_owner == this) key_owner = nullptr;
    }
  }

  void ToggleLyrics() {
    if (!cfg.lyrics_enabled) return;
    lyrics_user_on = !lyrics_user_on;
    cfg.lyrics_mode = lyrics_user_on;
    SaveSettingBool(L"lyrics_mode", lyrics_user_on);   // survives restarts; the watcher reloads the same value
    Logf(lyrics_user_on ? L"[lyrics] Ctrl+L: lyrics mode on\r\n" : L"[lyrics] Ctrl+L: lyrics mode off\r\n");
    static_dirty = true;
    major_anim = true;
  }

  // Foreground changes: the taskbar re-raises itself above the capsule when you switch apps, which
  // made the capsule vanish until the next z-order re-assert. The event kicks a short burst of
  // fast re-asserts instead (delivered on the UI thread through the loader's message loop).
  inline static Impl* fg_owner = nullptr;
  HWINEVENTHOOK fg_hook = nullptr;
  double z_burst = 0.0;   // seconds of fast z-order re-asserting left
  double hot_hold = 0.0;  // > 0 while the cursor is over the taskbar / its thumbnail area (+ a short tail)

  // Windows re-raises the taskbar over every other topmost window whenever it shows a taskbar thumbnail, a
  // jump list or a flyout, or when you click a taskbar button. Until we put the capsule back on top it is
  // hidden behind the taskbar = a visible blink. While the cursor is in that area the loop therefore wakes
  // every few ms and this cheap check (a short GetWindow walk, no drawing) puts the capsule back at once.
  void GuardZ() {
    if (!hwnd || !shown || hidden || !tb_cached) return;
    if (hot_hold <= 0.0 && z_burst <= 0.0) return;
    if (TaskbarAbove(hwnd, tb_cached)) {
      SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
      since_zorder = 0.0;
    }
  }

  // Normal pacing (the guard may wake the loop faster than this; see PillWindow::Tick).
  unsigned BaseWaitMs() const {
    if (z_burst > 0.0) return 16;
    if (hidden) return fullscreen ? 50 : 250;
    if (major_anim) return 16;
    if (viz_anim || pending_viz) return 33;
    // Synced lyrics: wake right when the next line is due instead of up to one idle tick late.
    if (lyr_next_ms >= 0 && lyr_next_ms < static_cast<int>(kIdleWaitMs))
      return static_cast<unsigned>((std::max)(4, lyr_next_ms + 1));
    return kIdleWaitMs;
  }

  static void CALLBACK ForegroundProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (fg_owner) fg_owner->z_burst = 0.8;
  }

  bool WheelHit(const POINT& pt) const {
    if (!shown || hidden || vis < 0.5) return false;
    const RECT r{ win_x + pad_px, win_y + pill_off_y, win_x + win_w - pad_px, win_y + pill_off_y + pill_h };
    return PtInRect(&r, pt) != FALSE;
  }

  static LRESULT CALLBACK LowLevelMouse(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL && hook_owner && hook_owner->hwnd) {
      const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
      const bool shift = Down(VK_SHIFT);
      // Only swallow a wheel event that one of the enabled features will actually use.
      const bool used = (shift && hook_owner->cfg.shift_scroll_skip) || hook_owner->cfg.scroll_volume;
      if (used && hook_owner->WheelHit(ms->pt)) {
        const short delta = static_cast<short>(HIWORD(ms->mouseData));
        PostMessageW(hook_owner->hwnd, kMsgWheel, static_cast<WPARAM>(static_cast<int>(delta)), shift ? 1 : 0);
        return 1;   // handled
      }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
  }

  void SyncWheelHook() {
    const bool want = (cfg.scroll_volume || cfg.shift_scroll_skip) && hwnd != nullptr;
    if (want && !wheel_hook) {
      hook_owner = this;
      wheel_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouse, GetModuleHandleW(nullptr), 0);
      Logf(wheel_hook ? L"[volume] mouse wheel capture on\r\n"
                      : (L"[volume] mouse wheel hook failed, error " + std::to_wstring(GetLastError()) + L"\r\n"));
    } else if (!want && wheel_hook) {
      UnhookWindowsHookEx(wheel_hook);
      wheel_hook = nullptr;
      if (hook_owner == this) hook_owner = nullptr;
    }
  }

  // Shift + wheel: skip tracks (wheel down = next). One skip per ~0.4 s so a fast spin / touchpad does not
  // run through the playlist.
  int skip_acc = 0;
  ULONGLONG skip_last = 0;
  void OnWheelSkip(int delta) {
    skip_acc += delta;
    const int notches = skip_acc / 120;
    if (notches == 0) return;
    skip_acc -= notches * 120;
    const ULONGLONG now = GetTickCount64();
    if (now - skip_last < 400) return;
    const int dir = notches > 0 ? -1 : +1;
    if (!media->has_session || !on_command) return;
    if (dir > 0 ? !media->can_next : !media->can_prev) return;
    skip_last = now;
    NotifySkip(dir);
    on_command(dir > 0 ? PillCommand::Next : PillCommand::Previous);
    Logf(dir > 0 ? L"[wheel] Shift+scroll: next\r\n" : L"[wheel] Shift+scroll: previous\r\n");
    major_anim = true;
  }

  void OnWheel(int delta, bool shift) {
    if (delta == 0) return;
    if (shift && cfg.shift_scroll_skip) {
      OnWheelSkip(delta);
      return;
    }
    if (!cfg.scroll_volume) return;
    wheel_acc += delta;
    const int notches = wheel_acc / 120;   // high-resolution wheels / touchpads accumulate
    if (notches == 0) return;
    wheel_acc -= notches * 120;

    const float step = static_cast<float>((std::clamp)(cfg.scroll_step, 1, 10)) / 100.0f;
    float level = 0.0f;
    bool muted = false;
    if (!sysvol.Nudge(static_cast<float>(notches) * step, &level, &muted)) {
      Logf(L"[volume] could not reach the default playback device\r\n");
      return;
    }
    Logf(L"[volume] " + std::to_wstring(static_cast<int>(std::lround(level * 100.0f))) + L"%" +
         (muted ? L" (muted)" : L"") + L"\r\n");

    if (vol_t < 0.05) vol_disp = level;   // HUD was hidden: appear at the right value
    vol_level = level;
    vol_muted = muted;
    vol_hold = 1.4;
    static_dirty = true;
    major_anim = true;
  }

  void NotifySkip(int dir) {
    pending_dir = dir;
    pending_age = 0.0;
  }

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
      case WM_MOUSEWHEEL:
        if (self) self->OnWheel(static_cast<int>(GET_WHEEL_DELTA_WPARAM(wp)), (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0);
        return 0;
      case kMsgWheel:
        if (self) self->OnWheel(static_cast<int>(static_cast<short>(wp)), lp != 0);
        return 0;
      case kMsgToggleLyrics:
        if (self) self->ToggleLyrics();
        return 0;
      case kMsgNudgeLyrics:
        if (self) self->NudgeLyrics(static_cast<int>(lp));
        return 0;
      case WM_SETTINGCHANGE:      // theme (ImmersiveColorSet), work area, ...
      case WM_DISPLAYCHANGE:
      case WM_DPICHANGED:
      case WM_THEMECHANGED:
        if (self) self->env_dirty = true;   // DPI + theme are re-read on the next tick
        break;                              // (DefWindowProc still handles the message)
      case WM_MOUSEMOVE:
      case WM_MOUSELEAVE:
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP:
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
      case WM_CAPTURECHANGED:
        if (self) self->OnMouse(msg, lp);
        return 0;
      default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
};

// ---------------------------------------------------------------------------

PillWindow::PillWindow() : impl_(std::make_unique<Impl>()) {}
PillWindow::~PillWindow() { Destroy(); }

void PillWindow::ApplySettings(const Settings& settings) {
  impl_->cfg = settings;
  impl_->cfg.max_width = (std::clamp)(impl_->cfg.max_width, 200, 600);
  impl_->lyrics_user_on = settings.lyrics_mode;
  impl_->content_dirty = true;
  impl_->static_dirty = true;
}

void PillWindow::SetAnchor(PillAnchor anchor, int margin_logical_px) {
  impl_->cfg.anchor = anchor;
  impl_->cfg.margin = margin_logical_px;
  impl_->content_dirty = true;
  impl_->static_dirty = true;
}

void PillWindow::SetLogHandler(std::function<void(const std::wstring&)> fn) {
  impl_->on_log = std::move(fn);
}

void PillWindow::SetCommandHandler(std::function<void(PillCommand)> fn) {
  impl_->on_command = std::move(fn);
}

void PillWindow::SetLyricsOffsetHandler(std::function<void(const std::wstring&, int)> fn) {
  impl_->offset_handler = std::move(fn);
}

void PillWindow::NotifySkip(int dir) { impl_->NotifySkip(dir); }

bool PillWindow::Create() {
  if (impl_->hwnd) return true;

  gp::GdiplusStartupInput in;
  if (gp::GdiplusStartup(&impl_->gdip_token, &in, nullptr) != gp::Ok) return false;
  impl_->gdip_ok = true;

  impl_->hinst = GetModuleHandleW(nullptr);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = Impl::WndProc;
  wc.hInstance = impl_->hinst;
  wc.lpszClassName = kClassName;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    Destroy();
    return false;
  }
  impl_->class_registered = true;

  // Not WS_EX_TRANSPARENT: the pill must receive hover/clicks. Fully transparent
  // pixels of a per-pixel-alpha layered window are click-through automatically.
  impl_->hwnd = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
      kClassName, L"Island Capsule", WS_POPUP,
      0, 0, 1, 1, nullptr, nullptr, impl_->hinst, nullptr);
  if (!impl_->hwnd) {
    Destroy();
    return false;
  }
  SetWindowLongPtrW(impl_->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl_.get()));

  Impl::fg_owner = impl_.get();
  impl_->fg_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, Impl::ForegroundProc, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  return true;
}

void PillWindow::Destroy() {
  if (!impl_) return;
  if (impl_->fg_hook) {
    UnhookWinEvent(impl_->fg_hook);
    impl_->fg_hook = nullptr;
    if (Impl::fg_owner == impl_.get()) Impl::fg_owner = nullptr;
  }
  if (impl_->key_hook) {
    UnhookWindowsHookEx(impl_->key_hook);
    impl_->key_hook = nullptr;
    if (Impl::key_owner == impl_.get()) Impl::key_owner = nullptr;
  }
  if (impl_->wheel_hook) {
    UnhookWindowsHookEx(impl_->wheel_hook);
    impl_->wheel_hook = nullptr;
    if (Impl::hook_owner == impl_.get()) Impl::hook_owner = nullptr;
  }
  if (impl_->hwnd) {
    SetWindowLongPtrW(impl_->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(impl_->hwnd);
    impl_->hwnd = nullptr;
  }
  impl_->fonts.reset();   // GDI+ objects must go before GdiplusShutdown
  impl_->ReleaseDrawCaches();
  impl_->ReleaseMeasure();
  impl_->ReleaseTarget();
  if (impl_->class_registered) {
    UnregisterClassW(kClassName, impl_->hinst);
    impl_->class_registered = false;
  }
  if (impl_->gdip_ok) {
    gp::GdiplusShutdown(impl_->gdip_token);
    impl_->gdip_ok = false;
  }
  impl_->shown = false;
}

namespace {
// Same on-screen content (everything except the timeline): a timeline-only update must not re-layout.
bool SameContent(const MediaInfo& a, const MediaInfo& b) {
  return a.has_session == b.has_session && a.is_playing == b.is_playing && a.can_play_pause == b.can_play_pause &&
         a.can_next == b.can_next && a.can_prev == b.can_prev && a.title == b.title && a.artist == b.artist &&
         a.album == b.album && a.source_app == b.source_app && a.art == b.art;
}
} // namespace

void PillWindow::SetMedia(std::shared_ptr<const MediaInfo> info) {
  if (!info) info = std::make_shared<const MediaInfo>();
  const bool same = SameContent(*impl_->media, *info);
  impl_->media = std::move(info);
  impl_->media_track_id = TrackId(*impl_->media);
  if (!same) impl_->content_dirty = true;
  impl_->static_dirty = true;
}

void PillWindow::SetLyrics(std::shared_ptr<const LyricsSnapshot> lyrics) {
  impl_->lyr = std::move(lyrics);
  impl_->static_dirty = true;
}

bool PillWindow::LyricsWanted() const { return impl_->LyricsWanted(); }

void PillWindow::Tick() {
  Impl& im = *impl_;
  im.GuardZ();   // cheap; runs on every wake-up, including the fast ones below
  // While the guard keeps the loop awake at ~4 ms, the full update + render still runs at its normal pace.
  if (im.hot_hold > 0.0 && im.shown && !im.hidden && im.have_qpc) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double since_ms = 1000.0 * static_cast<double>(now.QuadPart - im.qpc_last.QuadPart) /
                            static_cast<double>(im.qpc_freq.QuadPart);
    if (since_ms + 1.0 < static_cast<double>(im.BaseWaitMs())) return;
  }
  im.Tick();
}

unsigned PillWindow::NextWaitMs() const {
  const unsigned base = impl_->BaseWaitMs();
  if (impl_->hot_hold > 0.0 && impl_->shown && !impl_->hidden) return (std::min)(base, kGuardWaitMs);
  return base;
}

bool PillWindow::NeedsFrames() const {
  return !impl_->hidden && (impl_->major_anim || impl_->viz_anim || impl_->pending_viz);
}

} // namespace island
