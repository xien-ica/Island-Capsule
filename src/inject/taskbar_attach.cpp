// Phase 0 – Taskbar attach (overlay glass pill)

#include "../../include/inject/taskbar_attach.hpp"

#include <ShellScalingApi.h>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "shcore.lib")

namespace island {

namespace {

constexpr wchar_t kClassName[] = L"IslandCapsulePill";
constexpr double kPillHeightLogical = 36.0;

void FillRoundedRect(uint32_t* pixels, int w, int h, int radius,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int dx = 0, dy = 0;
      if (x < radius && y < radius) {
        dx = radius - x; dy = radius - y;
      } else if (x >= w - radius && y < radius) {
        dx = x - (w - radius - 1); dy = radius - y;
      } else if (x < radius && y >= h - radius) {
        dx = radius - x; dy = y - (h - radius - 1);
      } else if (x >= w - radius && y >= h - radius) {
        dx = x - (w - radius - 1); dy = y - (h - radius - 1);
      }

      bool inside = true;
      if (dx || dy) {
        inside = (dx * dx + dy * dy) <= (radius * radius);
      }

      if (inside) {
        uint8_t pr = static_cast<uint8_t>((r * a) / 255);
        uint8_t pg = static_cast<uint8_t>((g * a) / 255);
        uint8_t pb = static_cast<uint8_t>((b * a) / 255);
        pixels[y * w + x] = (static_cast<uint32_t>(a) << 24) |
                            (static_cast<uint32_t>(pr) << 16) |
                            (static_cast<uint32_t>(pg) << 8)  |
                             static_cast<uint32_t>(pb);
      } else {
        pixels[y * w + x] = 0;
      }
    }
  }
}

bool IsSystemDarkMode() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &key) != ERROR_SUCCESS) {
    return true;
  }
  DWORD value = 1;
  DWORD size = sizeof(value);
  RegQueryValueExW(key, L"SystemUsesLightTheme", nullptr, nullptr,
                   reinterpret_cast<LPBYTE>(&value), &size);
  RegCloseKey(key);
  return value == 0;
}

} // namespace

TaskbarAttach::~TaskbarAttach() { Detach(); }

bool TaskbarAttach::Attach() {
  if (hwnd_) return true;

  taskbar_hwnd_ = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (!taskbar_hwnd_) return false;

  dark_mode_ = IsSystemDarkMode();
  if (!CreatePillWindow()) return false;

  PositionOverTaskbar();
  RenderPill();
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  return true;
}

void TaskbarAttach::Detach() {
  DestroyPillWindow();
  taskbar_hwnd_ = nullptr;
}

void TaskbarAttach::Tick() {
  if (!hwnd_) return;
  HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (tb) taskbar_hwnd_ = tb;
  PositionOverTaskbar();
}

bool TaskbarAttach::CreatePillWindow() {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClassName;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);

  hwnd_ = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
      kClassName, L"Island Capsule", WS_POPUP,
      0, 0, width_, height_,
      nullptr, nullptr, wc.hInstance, this);

  return hwnd_ != nullptr;
}

void TaskbarAttach::DestroyPillWindow() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void TaskbarAttach::PositionOverTaskbar() {
  if (!hwnd_ || !taskbar_hwnd_) return;

  RECT rc{};
  GetWindowRect(taskbar_hwnd_, &rc);

  UINT dpiX = 96, dpiY = 96;
  HMONITOR mon = MonitorFromWindow(taskbar_hwnd_, MONITOR_DEFAULTTONEAREST);
  if (mon) {
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
  }
  dpi_ = static_cast<int>(dpiX);

  const double scale = dpi_ / 96.0;
  height_ = static_cast<int>(kPillHeightLogical * scale);
  width_  = static_cast<int>(160 * scale);

  const int tbH = rc.bottom - rc.top;
  const int x = rc.left + ((rc.right - rc.left) - width_) / 2;
  const int y = rc.top + (tbH - height_) / 2;

  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  // Re-render after size change
  UpdateLayeredBitmap();
}

void TaskbarAttach::RenderPill() {
  UpdateLayeredBitmap();
}

void TaskbarAttach::UpdateLayeredBitmap() {
  if (!hwnd_) return;

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width_;
  bmi.bmiHeader.biHeight = -height_;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (mem) DeleteDC(mem);
    if (screen) ReleaseDC(nullptr, screen);
    return;
  }

  HGDIOBJ old = SelectObject(mem, dib);
  auto* pixels = static_cast<uint32_t*>(bits);
  const int radius = height_ / 2;

  if (dark_mode_) {
    FillRoundedRect(pixels, width_, height_, radius, 0x20, 0x20, 0x24, 153);
  } else {
    FillRoundedRect(pixels, width_, height_, radius, 0xF4, 0xF4, 0xF6, 102);
  }

  POINT ptSrc{0, 0};
  SIZE size{width_, height_};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;

  POINT ptDst{};
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  ptDst.x = wr.left;
  ptDst.y = wr.top;

  UpdateLayeredWindow(hwnd_, screen, &ptDst, &size, mem, &ptSrc, 0, &blend, ULW_ALPHA);

  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK TaskbarAttach::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  TaskbarAttach* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
    self = static_cast<TaskbarAttach*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<TaskbarAttach*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }

  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;  // click-through for Phase 0
    case WM_DESTROY:
      if (self) self->hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static TaskbarAttach g_attach;

} // namespace island

// Exports must be outside the C++ namespace so names stay undecorated
static island::TaskbarAttach& GetAttach() {
  static island::TaskbarAttach instance;
  return instance;
}

extern "C" {

__declspec(dllexport) bool Island_Attach() {
  return GetAttach().Attach();
}

__declspec(dllexport) void Island_Detach() {
  GetAttach().Detach();
}

__declspec(dllexport) void Island_Tick() {
  GetAttach().Tick();
}

} // extern "C"
