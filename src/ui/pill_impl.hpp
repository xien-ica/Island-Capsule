#pragma once
// Private header shared by the pill_*.cpp files: the pill window's implementation struct (state + member
// declarations) and the small helper types it owns. Not part of the public interface (see pill.hpp).
//   pill.cpp         - public PillWindow API, layout, animation, lyrics logic, tick loop
//   pill_render.cpp  - drawing: text, capsule surface, cover, volume HUD, visualizer, layered-window target
//   pill_input.cpp   - window procedure, mouse, wheel / keyboard / foreground hooks
//   pill_menu.cpp    - right-click context menu

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

namespace gp = Gdiplus;

namespace island {

inline constexpr wchar_t kClassName[] = L"IslandCapsulePill";

// Logical (96-dpi) metrics
inline constexpr double kPillH     = 42.0;
inline constexpr double kMinW      = 120.0;
inline constexpr double kArtSize   = 32.0;
inline constexpr double kPadLeft   = 10.0;  // generous left margin so the cover clears the round end
inline constexpr double kGap       = 8.0;
inline constexpr double kPadRight  = 16.0;   // right padding when the visualizer is off
inline constexpr double kPadRightViz = 12.0; // right padding when the visualizer is on
inline constexpr double kVizGap    = 8.0;    // text -> visualizer slot
inline constexpr double kTitlePx   = 14.0;   // whole pixels render crisper than fractional sizes
inline constexpr double kArtistPx  = 12.0;
inline constexpr double kArtRadius = 6.5;
inline constexpr double kFadeW     = 14.0;   // marquee edge fade
inline constexpr double kHoverHold = 0.25;   // seconds the strip stays after the mouse leaves
inline constexpr double kWinPad    = 8.0;    // transparent margin around the content (hover scale room)
inline constexpr double kHoverGrow = 0.03;   // the whole capsule (text included) scales up by this much on hover
inline constexpr double kHudMinAvail = 112.0; // text-area width the volume HUD needs (speaker + bar + percent)
inline constexpr double kSlideDur  = 0.32;   // text slide on track change
inline constexpr double kSlideOff  = 14.0;   // logical px travelled by the slide
inline constexpr double kArtFade   = 0.35;   // cover crossfade
inline constexpr double kIdleHide  = 1.5;    // seconds without a session before the capsule fades out
inline constexpr double kVizFrame  = 0.030;  // visualizer-only redraw interval (~30 fps)
inline constexpr double kVizMargin = 0.20;   // visualizer repaint rect = slot + this fraction of the slot's long side (glow halo), + 2 px
inline constexpr double kEnvPoll   = 1.0;    // DPI / theme safety-net re-read (WM_SETTINGCHANGE etc. trigger it sooner)
inline constexpr unsigned kGuardWaitMs = 4;  // z-order guard cadence while the cursor is over the taskbar (see GuardZ)
inline constexpr double kHotHold = 0.5;      // keep the guard on this long after the cursor leaves the taskbar area
inline constexpr double kHotAboveDp = 340.0; // how far above the taskbar counts as "hot" (thumbnail / jump-list height), logical px
inline constexpr unsigned kIdleWaitMs = 250; // loop sleep while fully idle: nothing animating, no countdown running
inline constexpr unsigned kBusyWaitMs = 50;  // loop sleep while a short countdown (hover hold, volume HUD, ...) is still running
inline constexpr int kNudgeStepMs = 100;      // Ctrl+Left / Ctrl+Right step for the per-song lyrics timing nudge
inline constexpr double kNudgeShow = 1.2;     // seconds the nudge indicator stays on the capsule
inline constexpr double kLyricSlide = 0.30;   // lyric line transition (seconds)
inline constexpr double kLyricSlideOff = 8.0; // logical px a lyric line travels while changing
inline constexpr double kLyricMaxW = 900.0;   // lyrics capsule may grow past max_width so the WHOLE line fits (no scrolling)
inline constexpr double kUpFillet = 7.0;      // concave fillet where the control tab melts into the capsule
inline constexpr double kLyricMinFrac = 0.70; // lyrics capsule never narrower than this fraction of max_width (no breathing)

// One text hint for measuring AND drawing capsule text (title, artist, lyrics, volume HUD). The capsule is a
// translucent layered surface, so ClearType is not an option; GridFit however snaps every glyph advance to whole
// pixels, which with Segoe UI Variable gives uneven spacing ("mi nd") and soft stems. Plain grayscale AA keeps the
// true advances. Switch to TextRenderingHintAntiAliasGridFit here to go back.
inline constexpr gp::TextRenderingHint kCapsuleTextHint = gp::TextRenderingHintAntiAlias;
inline constexpr UINT kCapsuleTextContrast = 6;   // 0..12 (default 4): higher = crisper, slightly bolder grayscale text

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

inline uint32_t HashText(const std::wstring& s) {
  uint32_t h = 2166136261u;
  for (wchar_t c : s) { h ^= static_cast<uint32_t>(c); h *= 16777619u; }
  return h ? h : 1u;
}

inline bool ArtValid(const ArtImage* img) {
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

struct PillWindow::Impl {
  ULONG_PTR gdip_token = 0;
  bool gdip_ok = false;
  HWND hwnd = nullptr;
  HINSTANCE hinst = nullptr;
  bool class_registered = false;

  Settings cfg;
  std::function<void(PillCommand)> on_command;
  std::function<void()> on_update;   // loader's media / lyrics update work; also driven from the context-menu loop
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
  void ResolveText();
  void UpdateArt(bool snap);
  void SetAccentTarget(const Accent& a);
  void UpdateAccentSource(bool snap);
  void Layout(bool snap);

  // ---- lyrics -------------------------------------------------------------

  bool LyricsWanted() const { return cfg.lyrics_enabled && lyrics_user_on; }

  void ReleaseMeasure() {
    measure_g.reset();   // GDI+ objects must go before GdiplusShutdown
    if (measure_dc) { DeleteDC(measure_dc); measure_dc = nullptr; }
  }
  float MeasureLyric(const std::wstring& str);
  void RefreshLyricMetrics();
  void SetLyricLine(const std::wstring& text, bool animate);

  int EffectiveOffsetMs() const { return (std::clamp)(cfg.lyrics_offset_ms + track_off_ms, -5000, 5000); }
  void NudgeLyrics(int step_ms);
  bool UpdateLyrics();
  double FrameDt();
  double dt_real = 0.0;         // real time since the previous tick (up to 1 s): for slow safety-net timers
  unsigned last_wait_ms = 0;    // what NextWaitMs() last told the loader to sleep
  bool Animate(double dt, bool want_hidden);
  TransportState CurrentTransport() const;

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
  bool DrawDwRun(gp::Graphics& g, const std::wstring& str, bool is_artist, const Rgb& ink, float alpha255,
                 float x, float y);
  void DrawLine(gp::Graphics& g, const std::wstring& str, const gp::Font& font, const Rgb& ink,
                float alpha, float x, float clip_x, float y, float avail, float line_h,
                float text_w, bool overflow, const Marquee* mq);
  void DrawTextBlock(gp::Graphics& g, float tx, float avail, float H, float xoff, float alpha,
                     const std::wstring& title, const std::wstring& artist, float tw, float aw,
                     bool tover, bool aover, bool is_dim, const Marquee* tmq, const Marquee* amq);

  // Main capsule surface (0,0 = pill top-left).
  // Width of the control tab above the capsule: its own width (3 buttons), clamped to the capsule's flat top.
  float UpTabWidth() const {
    return (std::min)(static_cast<float>(strip.UpWidth()), static_cast<float>(pill_w - pill_h));
  }
  void BuildUnionPath(gp::GraphicsPath& path, float W, float H, float U, float tw) const;
  void DrawSurface(gp::Graphics& g, float W, float H);
  void DrawSeam(gp::Graphics& g, float alpha);
  void DrawLyricLine(gp::Graphics& g, float tx, float avail, float H);
  void DrawNudge(gp::Graphics& g, float tx, float avail, float H);
  void DrawVolume(gp::Graphics& g, float tx, float avail, float H);
  void DrawPill(gp::Graphics& g);
  void SetupGraphics(gp::Graphics& g) const;

  // Hover scale: one transform for the whole capsule group (1.0 = none).
  float RootScale() const {
    const float sc = 1.0f + static_cast<float>(kHoverGrow * hov_sp.x);
    return std::fabs(sc - 1.0f) > 0.0005f ? sc : 1.0f;
  }
  void ApplyRootTransform(gp::Graphics& g) const;
  void VizOrigin(float& vx, float& vy) const;
  RECT VizRect() const;
  void DrawViz(gp::Graphics& g, const RECT& clip);
  void Draw(gp::Graphics& g);

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
  SnapKey MakeKey() const;
  void ReleaseTarget();   // GDI+ objects must go before GdiplusShutdown
  bool EnsureTarget();
  void Render(bool viz_only = false);
  int AnchorX(HWND taskbar, const RECT& rc, int w) const;
  void Tick();
  int HitButton(int mx, int my) const;
  bool ButtonEnabled(int b) const;
  void OnMouse(UINT msg, LPARAM lp);

  // ---- click actions -------------------------------------------------------------------------
  bool mid_down = false, right_down = false;
  bool CopyText(const std::wstring& text);

  // Right-click menu – custom-drawn to match the settings window (same palette, fonts, rounding).
  // Modal: runs its own message loop until the user picks an item or clicks away.
  // Right-click menu – custom-drawn to match the settings window (same palette, fonts, rounding).
  // Layered popup + UpdateLayeredWindow so corners are clean (no region / no GDI ghosting).
  // Modal: own message loop until the user picks an item or clicks away.
  bool menu_open = false;          // a context menu is already showing (its loop is running)
  HWND menu_hwnd_active = nullptr; // so a second right-click can dismiss it instead of stacking another
  void ShowContextMenu();

  // ---- wheel capture -------------------------------------------------------------------------
  // WM_MOUSEWHEEL only reaches this no-activate window if Windows' "scroll inactive windows"
  // routing happens to apply, so a low-level mouse hook catches the wheel while the cursor is over
  // the capsule (and swallows it so the app underneath does not scroll as well). The hook is installed
  // ONLY while the cursor is over the capsule (hover) and removed when it leaves: a global WH_MOUSE_LL hook
  // is called for every mouse move on the system, so it must not stay installed all the time.
  static constexpr UINT kMsgWheel = WM_APP + 21;
  inline static Impl* hook_owner = nullptr;
  HHOOK wheel_hook = nullptr;
  bool wheel_fail_logged = false;

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
  static LRESULT CALLBACK LowLevelKeyboard(int code, WPARAM wp, LPARAM lp);
  void SyncKeyHook();
  void ToggleLyrics();

  // Foreground changes: the taskbar re-raises itself above the capsule when you switch apps, which
  // made the capsule vanish until the next z-order re-assert. The event kicks a short burst of
  // fast re-asserts instead (delivered on the UI thread through the loader's message loop).
  inline static Impl* fg_owner = nullptr;
  HWINEVENTHOOK fg_hook = nullptr;
  double z_burst = 0.0;   // seconds of fast z-order re-asserting left
  double hot_hold = 0.0;  // > 0 while the cursor is over the taskbar / its thumbnail area (+ a short tail)
  void GuardZ();
  unsigned BaseWaitMs() const;
  bool TimersPending() const;

  static void CALLBACK ForegroundProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (fg_owner) fg_owner->z_burst = 0.8;
  }

  bool WheelHit(const POINT& pt) const {
    if (!shown || hidden || vis < 0.5) return false;
    const RECT r{ win_x + pad_px, win_y + pill_off_y, win_x + win_w - pad_px, win_y + pill_off_y + pill_h };
    return PtInRect(&r, pt) != FALSE;
  }
  static LRESULT CALLBACK LowLevelMouse(int code, WPARAM wp, LPARAM lp);
  void SyncWheelHook();

  // Shift + wheel: skip tracks (wheel down = next). One skip per ~0.4 s so a fast spin / touchpad does not
  // run through the playlist.
  int skip_acc = 0;
  ULONGLONG skip_last = 0;
  void OnWheelSkip(int delta);
  void OnWheel(int delta, bool shift);
  void NotifySkip(int dir);
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
};

} // namespace island
