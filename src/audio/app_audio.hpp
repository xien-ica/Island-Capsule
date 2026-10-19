#pragma once
// Helpers that connect a media session's app id (MediaInfo::source_app, an AppUserModelId such as "Spotify.exe",
// "Chrome", "MSEdge" or "SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify") to the Windows processes that play its
// audio. Shared by the per-app volume (volume.cpp) and the per-app visualizer (spectrum.cpp).
// Plain interface: no Windows types leak out of the header.

#include <string>

namespace island {

std::wstring LowerW(std::wstring s);

// Process id -> lower-case executable name without the extension ("C:\...\Spotify.exe" -> "spotify").
// Empty when the process cannot be opened (protected / already gone).
std::wstring ExeStem(unsigned long pid);

// Does the app id (already lower-cased) belong to the process whose executable stem is `stem`? A loose,
// case-insensitive containment check in either direction; very short names never match.
bool AppMatchesExe(const std::wstring& app_lower, const std::wstring& stem);

// The process id to capture / control for `source_app`: the ROOT of the app's process tree (a browser plays its
// audio from a helper process, but process-loopback capture of the root includes all of its children).
// Prefers a process that currently owns an audio session (actively playing first). Returns 0 when no matching
// process is running. Call from a thread with COM initialised (the audio-session lookup uses it; without COM the
// lookup is skipped and the largest matching process tree is used).
unsigned long FindAppRootPid(const std::wstring& source_app);

} // namespace island
