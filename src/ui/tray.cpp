#include "ui/tray.hpp"

#include <shellapi.h>

#include <wchar.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "settings/settings.hpp"
#include "ui/settings_window.hpp"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace island {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"IslandCapsule";
constexpr UINT kTrayMsg = WM_APP + 10;
constexpr UINT kTrayId = 1;

enum MenuId : UINT { kMenuSettings = 1, kMenuAutostart = 2, kMenuQuit = 3 };

std::wstring ExePath() {
  wchar_t buf[MAX_PATH * 2] = {};
  const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
  return std::wstring(buf, n);
}

// A tiny capsule drawn from a signed-distance function: no image asset needed.
HICON MakeIconImpl(int sz) {
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = sz;
  bmi.bmiHeader.biHeight = -sz;   // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!color || !bits) {
    if (color) DeleteObject(color);
    return nullptr;
  }

  const float S = static_cast<float>(sz);
  const float h = S * 0.62f;                 // capsule height
  const float w = S - 2.0f;                  // capsule width
  const float cx = S * 0.5f, cy = S * 0.5f;
  const float r = h * 0.5f;
  const float dot_x = cx - w * 0.5f + r;     // "cover" circle on the left end
  const float dot_r = r * 0.62f;

  auto* px = static_cast<uint32_t*>(bits);
  for (int y = 0; y < sz; ++y) {
    for (int x = 0; x < sz; ++x) {
      const float fx = x + 0.5f, fy = y + 0.5f;
      // capsule SDF (horizontal segment of half-length w/2 - r, radius r)
      const float half = w * 0.5f - r;
      const float qx = (std::max)(std::fabs(fx - cx) - half, 0.0f);
      const float qy = fy - cy;
      const float d = std::sqrt(qx * qx + qy * qy) - r;
      const float cov = (std::clamp)(0.5f - d, 0.0f, 1.0f);

      const float dd = std::sqrt((fx - dot_x) * (fx - dot_x) + (fy - cy) * (fy - cy)) - dot_r;
      const float dot = (std::clamp)(0.5f - dd, 0.0f, 1.0f);

      // light capsule, accent-blue cover circle
      const float R = 236.0f * (1.0f - dot) + 96.0f * dot;
      const float G = 236.0f * (1.0f - dot) + 120.0f * dot;
      const float B = 240.0f * (1.0f - dot) + 255.0f * dot;
      const uint32_t a = static_cast<uint32_t>(cov * 255.0f + 0.5f);
      px[static_cast<size_t>(y) * sz + x] = (a << 24) | (static_cast<uint32_t>(R) << 16) |
                                            (static_cast<uint32_t>(G) << 8) | static_cast<uint32_t>(B);
    }
  }

  HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmColor = color;
  ii.hbmMask = mask;
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(color);
  DeleteObject(mask);
  return icon;
}

} // namespace

HICON CreateAppIcon(int size) { return MakeIconImpl(size); }

bool IsAutostartEnabled() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
  const bool present = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
  RegCloseKey(key);
  return present;
}

bool SetAutostartEnabled(bool on) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS)
    return false;
  bool ok;
  if (on) {
    const std::wstring cmd = L"\"" + ExePath() + L"\" --autostart";
    ok = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                        static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
  } else {
    const LONG r = RegDeleteValueW(key, kRunValue);
    ok = r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
  }
  RegCloseKey(key);
  return ok;
}

// ---------------------------------------------------------------------------

struct TrayIcon::Impl {
  HINSTANCE hinst = nullptr;
  HWND hwnd = nullptr;
  HICON icon = nullptr;
  bool class_registered = false;
  bool added = false;
  UINT taskbar_created = 0;

  void AddIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = icon;
    wcsncpy_s(nid.szTip, L"Island Capsule", _TRUNCATE);
    added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
  }

  void RemoveIcon() {
    if (!added) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added = false;
  }

  void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Open settings");
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : MF_UNCHECKED), kMenuAutostart,
                L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);   // required so the menu closes when you click elsewhere
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                                      pt.x, pt.y, 0, hwnd, nullptr));
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
      case kMenuSettings:
        OpenSettingsWindow(hinst);
        break;
      case kMenuAutostart:
        SetAutostartEnabled(!IsAutostartEnabled());
        break;
      case kMenuQuit:
        PostQuitMessage(0);   // the loader's loop sees WM_QUIT and shuts down cleanly
        break;
      default:
        break;
    }
  }

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
      if (msg == TrayIcon::kQuitMsg) {
        PostQuitMessage(0);
        return 0;
      }
      if (msg == kTrayMsg) {
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) self->ShowMenu();
        return 0;
      }
      if (msg == self->taskbar_created) {   // explorer restarted: the icon has to be re-added
        self->added = false;
        self->AddIcon();
        return 0;
      }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
};

TrayIcon::TrayIcon() : impl_(std::make_unique<Impl>()) {}
TrayIcon::~TrayIcon() { Destroy(); }

bool TrayIcon::Visible() const { return impl_ && impl_->added; }

bool TrayIcon::Create(HINSTANCE hinst) {
  Impl& d = *impl_;
  if (d.hwnd) return true;
  d.hinst = hinst;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = Impl::WndProc;
  wc.hInstance = hinst;
  wc.lpszClassName = TrayIcon::kWindowClass;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
  d.class_registered = true;

  // A hidden *top-level* window (not message-only): only those receive the "TaskbarCreated" broadcast.
  d.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, TrayIcon::kWindowClass, L"Island Capsule tray", WS_POPUP, 0, 0, 0, 0, nullptr,
                           nullptr, hinst, nullptr);
  if (!d.hwnd) {
    Destroy();
    return false;
  }
  SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&d));
  d.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

  d.icon = MakeIconImpl((std::max)(16, GetSystemMetrics(SM_CXSMICON)));
  if (!d.icon) d.icon = LoadIconW(nullptr, IDI_APPLICATION);
  d.AddIcon();
  return true;
}

void TrayIcon::Destroy() {
  if (!impl_) return;
  Impl& d = *impl_;
  d.RemoveIcon();
  if (d.hwnd) {
    SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, 0);
    DestroyWindow(d.hwnd);
    d.hwnd = nullptr;
  }
  if (d.icon) {
    DestroyIcon(d.icon);   // harmless if it was the shared IDI_APPLICATION handle
    d.icon = nullptr;
  }
  if (d.class_registered) {
    UnregisterClassW(TrayIcon::kWindowClass, d.hinst);
    d.class_registered = false;
  }
}

} // namespace island
