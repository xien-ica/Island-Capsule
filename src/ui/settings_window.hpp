#pragma once
// Settings window: a small, clean replacement for opening settings.ini in Notepad.
//
// Native Win32 + GDI+ (no extra dependencies). Follows the system light / dark theme and DPI.
// Every change is written straight to settings.ini (only the one key that changed), and the
// running loader hot-reloads the file, so changes show up on the capsule within ~1 s.
//
// UI thread only. The window has no message loop of its own: it relies on the loader's loop.

#include <windows.h>

namespace island {

// Opens the settings window, or brings the existing one to the front.
void OpenSettingsWindow(HINSTANCE hinst);

// Closes the window if it is open (called on shutdown).
void CloseSettingsWindow();

} // namespace island
