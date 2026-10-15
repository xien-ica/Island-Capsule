// Pill window: input. Window procedure, mouse handling, and the low-level mouse / keyboard hooks plus the
// foreground-change event hook that exist only while they are needed.

#include "ui/pill_impl.hpp"

namespace island {

// ---- mouse --------------------------------------------------------------

int PillWindow::Impl::HitButton(int mx, int my) const {
  if (ExpandUp() >= 0.6) {   // the panel above the capsule (lyrics mode)
    if (my >= pill_off_y) return -1;
    const int tx0 = pill_off_x + static_cast<int>(std::lround((pill_w - UpTabWidth()) * 0.5f));
    return strip.HitTestUp(mx - tx0, my - (pill_off_y - up_extra_px), static_cast<int>(std::lround(UpTabWidth())));
  }
  if (Expand() < 0.6) return -1;
  return strip.HitTest(mx - strip_x, my - pill_off_y);
}

bool PillWindow::Impl::ButtonEnabled(int b) const {
  const MediaInfo& m = *media;
  if (b == TransportStrip::kPrev) return m.can_prev;
  if (b == TransportStrip::kPlayPause) return m.can_play_pause;
  if (b == TransportStrip::kNext) return m.can_next;
  return false;
}

void PillWindow::Impl::OnMouse(UINT msg, LPARAM lp) {
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
  SyncWheelHook();     // so does the wheel capture
  static_dirty = true;
  major_anim = true;   // make sure the next frames run at full pace
}

LRESULT CALLBACK PillWindow::Impl::LowLevelKeyboard(int code, WPARAM wp, LPARAM lp) {
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

void PillWindow::Impl::SyncKeyHook() {
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

void PillWindow::Impl::ToggleLyrics() {
  if (!cfg.lyrics_enabled) return;
  lyrics_user_on = !lyrics_user_on;
  cfg.lyrics_mode = lyrics_user_on;
  SaveSettingBool(L"lyrics_mode", lyrics_user_on);   // survives restarts; the watcher reloads the same value
  Logf(lyrics_user_on ? L"[lyrics] Ctrl+L: lyrics mode on\r\n" : L"[lyrics] Ctrl+L: lyrics mode off\r\n");
  static_dirty = true;
  major_anim = true;
}

LRESULT CALLBACK PillWindow::Impl::LowLevelMouse(int code, WPARAM wp, LPARAM lp) {
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

void PillWindow::Impl::SyncWheelHook() {
  const bool want = (cfg.scroll_volume || cfg.shift_scroll_skip) && hwnd != nullptr && shown && !hidden && hover;
  if (want && !wheel_hook) {
    hook_owner = this;
    wheel_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouse, GetModuleHandleW(nullptr), 0);
    if (!wheel_hook && !wheel_fail_logged) {   // success is not logged: this runs on every hover
      wheel_fail_logged = true;
      Logf(L"[volume] mouse wheel hook failed, error " + std::to_wstring(GetLastError()) + L"\r\n");
    }
  } else if (!want && wheel_hook) {
    UnhookWindowsHookEx(wheel_hook);
    wheel_hook = nullptr;
    if (hook_owner == this) hook_owner = nullptr;
  }
}

void PillWindow::Impl::OnWheelSkip(int delta) {
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

void PillWindow::Impl::OnWheel(int delta, bool shift) {
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

void PillWindow::Impl::NotifySkip(int dir) {
  pending_dir = dir;
  pending_age = 0.0;
}

LRESULT CALLBACK PillWindow::Impl::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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

} // namespace island
