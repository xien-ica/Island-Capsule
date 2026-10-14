#pragma once

#include <windows.h>

namespace island {

// Phase 0: docked overlay pill on the primary taskbar.
class TaskbarAttach {
public:
  TaskbarAttach() = default;
  ~TaskbarAttach();

  bool Attach();
  void Detach();
  bool IsAttached() const { return hwnd_ != nullptr; }
  void Tick();

private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

  bool CreatePillWindow();
  void DestroyPillWindow();
  void PositionOverTaskbar();
  void RenderPill();
  void UpdateLayeredBitmap();

  HWND hwnd_ = nullptr;
  HWND taskbar_hwnd_ = nullptr;
  int width_ = 160;
  int height_ = 36;
  int dpi_ = 96;
  bool dark_mode_ = true;
};

extern "C" {
  __declspec(dllexport) bool Island_Attach();
  __declspec(dllexport) void Island_Detach();
  __declspec(dllexport) void Island_Tick();
}

} // namespace island