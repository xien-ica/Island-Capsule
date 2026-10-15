// Pill window: right-click context menu (its own small layered window + modal loop).

#include "ui/pill_impl.hpp"

namespace island {

bool PillWindow::Impl::CopyText(const std::wstring& text) {
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

void PillWindow::Impl::ShowContextMenu() {
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
    if (!st.done) {
      if (on_update) on_update();   // media / lyrics changes keep showing while the menu is open
      Tick();
    }
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

} // namespace island
