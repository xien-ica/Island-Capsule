// Pill window: GDI+ rendering into a premultiplied 32-bit DIB, shown with
// UpdateLayeredWindow so the rounded edges are properly anti-aliased.
// Polish pass: the transport strip is its own slightly shorter capsule tucked UNDER the main pill,
// no scaling anywhere (everything is pixel-aligned), cover pre-downsampled to its exact on-screen
// size, integer-snapped text.

#include "ui/pill_impl.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace island {

namespace {

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

} // namespace

// ---- text ---------------------------------------------------------------

void PillWindow::Impl::ResolveText() {
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

void PillWindow::Impl::UpdateArt(bool snap) {
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

void PillWindow::Impl::SetAccentTarget(const Accent& a) {
  acc_tgt = a.color;
  str_tgt = a.strength;
}

void PillWindow::Impl::UpdateAccentSource(bool snap) {
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
void PillWindow::Impl::Layout(bool snap) {
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

float PillWindow::Impl::MeasureLyric(const std::wstring& str) {
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
void PillWindow::Impl::RefreshLyricMetrics() {
  if (lyr_text.empty()) return;
  const double s = Scale();
  lyr_text_w = MeasureLyric(lyr_text);
  const double fixed = (kPadLeft + kArtSize + kGap) * s + RightReservePx();
  const double hi = (std::max)(MaxW(), kLyricMaxW) * s;   // full line always fits (practically)
  const double lo = (std::min)(hi, (std::max)(kMinW, MaxW() * kLyricMinFrac) * s);
  lyr_target_w = std::round((std::clamp)(fixed + std::ceil(lyr_text_w) + 2.0, lo, hi));
  lyr_over = false;   // lyrics never scroll
}

void PillWindow::Impl::SetLyricLine(const std::wstring& text, bool animate) {
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

// Ctrl+Left / Ctrl+Right (step_ms = -/+kNudgeStepMs) or Ctrl+Down (step_ms = 0: reset this song's nudge).
// Only the per-song part changes; the global lyrics_offset_ms stays in the settings window.
void PillWindow::Impl::NudgeLyrics(int step_ms) {
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
bool PillWindow::Impl::UpdateLyrics() {
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

double PillWindow::Impl::FrameDt() {
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
  dt_real = (std::min)((std::max)(dt, 0.0), 1.0);
  // First frame after a long idle sleep: an animation must not start 0.1 s "in" (it would skip its first third).
  // Safe for timers because the idle pace is only used while none is counting down (see TimersPending).
  if (last_wait_ms >= kIdleWaitMs && dt > 0.06) return 1.0 / 60.0;
  return (std::min)((std::max)(dt, 0.0), 0.1);
}

// Everything except the visualizer. Returns true if a redraw is needed.
bool PillWindow::Impl::Animate(double dt, bool want_hidden) {
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

TransportState PillWindow::Impl::CurrentTransport() const {
  TransportState t;
  t.playing = media->is_playing;
  t.can_prev = media->can_prev;
  t.can_play_pause = media->can_play_pause;
  t.can_next = media->can_next;
  return t;
}

// ---- placement ----------------------------------------------------------

int PillWindow::Impl::AnchorX(HWND taskbar, const RECT& rc, int w) const {
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

void PillWindow::Impl::Tick() {
  if (!hwnd) return;
  SyncWheelHook();
  SyncKeyHook();

  const double dt = FrameDt();
  since_render += dt;
  since_zorder += dt;
  env_since += dt_real;
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
  audio.SetTargetApp(cfg.visualizer_app_only ? media->source_app : std::wstring());
  audio.SetActive(cfg.visualizer_audio && viz.Enabled() && media->has_session && media->is_playing && !want_hidden);
  {
    std::wstring audio_note;
    if (audio.TakeDiagnostic(&audio_note)) Logf(L"[visualizer] " + audio_note + L"\r\n");
  }
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

// Windows re-raises the taskbar over every other topmost window whenever it shows a taskbar thumbnail, a
// jump list or a flyout, or when you click a taskbar button. Until we put the capsule back on top it is
// hidden behind the taskbar = a visible blink. While the cursor is in that area the loop therefore wakes
// every few ms and this cheap check (a short GetWindow walk, no drawing) puts the capsule back at once.
void PillWindow::Impl::GuardZ() {
  if (!hwnd || !shown || hidden || !tb_cached) return;
  if (hot_hold <= 0.0 && z_burst <= 0.0) return;
  if (TaskbarAbove(hwnd, tb_cached)) {
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    since_zorder = 0.0;
  }
}

// Normal pacing (the guard may wake the loop faster than this; see PillWindow::Tick).
unsigned PillWindow::Impl::BaseWaitMs() const {
  if (z_burst > 0.0) return 16;
  if (hidden) return kIdleWaitMs;   // a fullscreen exit is caught by the foreground event, or by the next idle tick
  if (major_anim) return 16;
  if (viz_anim || pending_viz) return 33;
  const unsigned base = TimersPending() ? kBusyWaitMs : kIdleWaitMs;
  // Synced lyrics: wake right when the next line is due instead of up to one idle tick late.
  if (lyr_next_ms >= 0 && lyr_next_ms < static_cast<int>(base))
    return static_cast<unsigned>((std::max)(4, lyr_next_ms + 1));
  return base;
}

// True while a countdown that only advances inside Tick() is still running. Tick() clamps dt, so sleeping
// the long idle interval would stretch such a timer; the loop keeps the finer pace until it has expired.
bool PillWindow::Impl::TimersPending() const {
  if (hold > 0.0 || vol_hold > 0.0 || vol_t > 0.01 || nudge_hold > 0.0 || nudge_t > 0.001) return true;
  if (media && !media->has_session && idle_for <= kIdleHide + 0.2) return true;          // about to fade out
  if (media && media->has_session && !media->is_playing && paused_for < 0.8) return true; // collapse debounce
  return false;
}


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

void PillWindow::SetUpdateHandler(std::function<void()> fn) {
  impl_->on_update = std::move(fn);
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
  // A timeline-only update (position / duration, sent often while lyrics mode is on) changes nothing that is drawn
  // here: the lyric line is picked every Tick. Marking it dirty forced a full redraw instead of the cheap
  // visualizer-only frame.
  if (!same) {
    impl_->content_dirty = true;
    impl_->static_dirty = true;
  }
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
  unsigned wait = impl_->BaseWaitMs();
  if (impl_->hot_hold > 0.0 && impl_->shown && !impl_->hidden) wait = (std::min)(wait, kGuardWaitMs);
  impl_->last_wait_ms = wait;
  return wait;
}

bool PillWindow::NeedsFrames() const {
  return !impl_->hidden && (impl_->major_anim || impl_->viz_anim || impl_->pending_viz);
}

} // namespace island
