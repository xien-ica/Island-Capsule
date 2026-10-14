#pragma once
// Tray icon with a small menu: Open settings, Start with Windows (toggle), Quit.
// UI thread only; the icon's messages arrive through the loader's normal message loop.

#include <windows.h>
#include <memory>

namespace island {

class TrayIcon {
public:
  TrayIcon();
  ~TrayIcon();
  TrayIcon(const TrayIcon&) = delete;
  TrayIcon& operator=(const TrayIcon&) = delete;

  bool Create(HINSTANCE hinst);
  void Destroy();
  bool Visible() const;   // the icon was accepted by the shell
  static constexpr UINT kQuitMsg = WM_APP + 11;   // posted by a newer instance to replace this one
  static constexpr const wchar_t* kWindowClass = L"IslandCapsuleTray";

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The capsule icon (same artwork as the tray). Caller owns the handle (DestroyIcon).
HICON CreateAppIcon(int size);

// "Start with Windows": HKCU\...\Run\IslandCapsule = "<this exe>" --autostart
bool IsAutostartEnabled();
bool SetAutostartEnabled(bool on);

} // namespace island
