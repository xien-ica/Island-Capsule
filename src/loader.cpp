// island-capsule – Phases 0-5: standalone pill + live media (GSMTC) + album color +
// in-pill visualizer + settings / motion polish.
//
//   island_loader.exe [left|center|right] [--prefer=spotify,chrome]
//                     [--viz=ticks|wave|glow|off] [--reduced-motion] [--autostart]
//
// Settings live in %APPDATA%\IslandCapsule\settings.ini (created on first run, hot-reloaded).
// Command-line options override the file.
//
// Hover the pill to reveal the transport strip (prev / play-pause / next).
// Ctrl+L while hovering the capsule toggles lyrics mode (synced lyrics from lrclib.net, see src/lyrics/lrclib.hpp).
// Console keys:  Space = play/pause   Left = previous   Right = next
//                Enter / Esc / Q = quit

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "island_version.hpp"

#include <wchar.h>

#include <atomic>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "crash.hpp"
#include "lyrics/lrclib.hpp"
#include "media/gsmtc.hpp"
#include "settings/settings.hpp"
#include "ui/pill.hpp"
#include "ui/settings_window.hpp"
#include "ui/tray.hpp"

namespace {

void Log(const std::wstring& text) {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!out || out == INVALID_HANDLE_VALUE) return;
  DWORD mode = 0;
  DWORD written = 0;
  if (GetConsoleMode(out, &mode)) {
    WriteConsoleW(out, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
  } else {
    // Redirected to a file/pipe: write UTF-8.
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n > 0) {
      std::string utf8(static_cast<size_t>(n), '\0');
      WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          utf8.data(), n, nullptr, nullptr);
      WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
  }
}

void LogMedia(const island::MediaInfo& m) {
  std::wostringstream o;
  if (!m.has_session) {
    o << L"[no media session]\r\n";
  } else {
    o << (m.is_playing ? L"[playing] " : L"[paused]  ")
      << (m.artist.empty() ? L"(unknown artist)" : m.artist) << L" - "
      << (m.title.empty() ? L"(unknown title)" : m.title)
      << L"   {" << m.source_app << L"}"
      << (m.art ? L"  art:yes" : L"  art:no")
      << L"  ctl:" << (m.can_prev ? L"<" : L"-") << (m.can_play_pause ? L"=" : L"-")
      << (m.can_next ? L">" : L"-") << L"\r\n";
  }
  Log(o.str());
}

// Returns false when the user asked to quit.
bool HandleConsoleInput(HANDLE in, island::MediaSession& media, island::PillWindow& pill) {
  DWORD pending = 0;
  if (!GetNumberOfConsoleInputEvents(in, &pending)) return true;
  while (pending--) {
    INPUT_RECORD rec{};
    DWORD read = 0;
    if (!ReadConsoleInputW(in, &rec, 1, &read) || read == 0) break;
    if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
    switch (rec.Event.KeyEvent.wVirtualKeyCode) {
      case VK_RETURN:
      case VK_ESCAPE:
      case 'Q':
        return false;
      case VK_SPACE: media.PlayPause(); break;
      case VK_LEFT:  pill.NotifySkip(-1); media.Previous(); break;
      case VK_RIGHT: pill.NotifySkip(+1); media.Next(); break;
      default: break;
    }
  }
  return true;
}

std::vector<std::wstring> SplitList(const std::wstring& s) {
  std::vector<std::wstring> out;
  std::wstring cur;
  for (wchar_t c : s) {
    if (c == L',' || c == L';') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// Only one capsule at a time: a newer instance asks the older one (via its tray window) to quit,
// so rebuilds never leave two overlapping capsules behind.
HANDLE TakeSingleInstance() {
  for (int i = 0; i < 40; ++i) {
    HANDLE h = CreateMutexW(nullptr, FALSE, L"Local\\IslandCapsule.SingleInstance");
    if (h && GetLastError() != ERROR_ALREADY_EXISTS) return h;
    if (h) CloseHandle(h);
    if (i == 0) {
      Log(L"Another Island Capsule is running - asking it to quit...\r\n");
      if (HWND old = FindWindowW(island::TrayIcon::kWindowClass, nullptr))
        PostMessageW(old, island::TrayIcon::kQuitMsg, 0, 0);
    }
    Sleep(100);
  }
  Log(L"The other instance did not quit (an older build without a tray icon?). Close it in Task Manager.\r\n");
  return nullptr;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  // First thing: if the process dies from an unhandled exception, write %APPDATA%\IslandCapsule\crash.log + crash.dmp.
  island::InstallCrashHandler();

  // Real pixel coordinates (no DPI virtualisation) so we line up with the taskbar.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // Command-line overrides (re-applied on top of every settings reload).
  struct Cli {
    bool has_anchor = false;
    island::PillAnchor anchor = island::PillAnchor::Center;
    bool has_viz = false;
    island::VizStyle viz = island::VizStyle::Ticks;
    bool reduced = false;
    bool autostart = false;   // launched by "Start with Windows": no console window
    bool has_prefer = false;
    std::vector<std::wstring> prefer;
  } cli;

  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    island::PillAnchor pa;
    island::VizStyle pv;
    if (island::ParseAnchor(a, &pa)) { cli.has_anchor = true; cli.anchor = pa; }
    else if (a.rfind(L"--viz=", 0) == 0 && island::ParseViz(a.substr(6), &pv)) { cli.has_viz = true; cli.viz = pv; }
    else if (a.rfind(L"--prefer=", 0) == 0) { cli.has_prefer = true; cli.prefer = SplitList(a.substr(9)); }
    else if (_wcsicmp(a.c_str(), L"--reduced-motion") == 0) cli.reduced = true;
    else if (_wcsicmp(a.c_str(), L"--autostart") == 0) cli.autostart = true;
  }

  auto apply_cli = [&](island::Settings& st) {
    if (cli.has_anchor) st.anchor = cli.anchor;
    if (cli.has_viz) st.visualizer = cli.viz;
    if (cli.reduced) st.reduced_motion = true;
    if (cli.has_prefer) st.preferred_apps = cli.prefer;
  };

  island::EnsureSettingsFile();
  island::SettingsWatcher watcher;
  island::Settings settings;
  watcher.Poll(&settings);   // primes the watcher + loads the file
  apply_cli(settings);

  Log(L"island-capsule loader v" ISLAND_VERSION_W L" - Phases 0-6 (lyrics)\r\n");
  Log(L"Settings: " + island::SettingsPath() + L"\r\n");
  Log(L"Keys: Space = play/pause, Left/Right = prev/next, Enter = quit\r\n\r\n");

  // Launched from the Run key: drop the console so nothing stays on screen.
  if (cli.autostart) FreeConsole();

  HANDLE single = TakeSingleInstance();

  island::PillWindow pill;
  pill.SetLogHandler([](const std::wstring& line) { Log(line); });
  pill.ApplySettings(settings);
  if (!pill.Create()) {
    Log(L"Could not create the pill window.\r\n");
    return 1;
  }

  island::TrayIcon tray;   // Open settings / Start with Windows / Quit
  if (!tray.Create(GetModuleHandleW(nullptr)))
    Log(L"[tray] could not create the tray window, error " + std::to_wstring(GetLastError()) + L"\r\n");
  else
    Log(tray.Visible() ? L"[tray] icon added (check the ^ overflow next to the clock)\r\n"
                       : L"[tray] icon not accepted by the shell yet - it will retry if explorer restarts\r\n");

  const DWORD main_tid = GetCurrentThreadId();
  std::atomic<bool> media_dirty{ true };

  island::MediaSession media;   // declared after media_dirty: destroyed first
  media.SetPreferredApps(settings.preferred_apps);
  media.SetSourceFilter(settings.selected_only, settings.selected_apps);
  if (!media.Start([&] {
        media_dirty = true;
        PostThreadMessageW(main_tid, WM_APP, 0, 0);   // wake the UI loop
      })) {
    Log(L"Could not start the media worker.\r\n");
    return 1;
  }

  // Lyrics (LRCLIB). Only active while the user wants lyrics mode: then the playback timeline is read
  // (internally, for timing) and lookups run on the fetcher's own thread. Declared after media_dirty.
  std::atomic<bool> lyrics_dirty{ false };
  island::LyricsFetcher lyrics;
  if (!lyrics.Start([&] {
        lyrics_dirty = true;
        PostThreadMessageW(main_tid, WM_APP, 0, 0);
      }))
    Log(L"[lyrics] could not start the lyrics worker\r\n");
  lyrics.SetCacheEnabled(settings.lyrics_cache);
  // Ctrl+Left / Ctrl+Right over the capsule: the pill keeps the per-song nudge and hands it over to be saved
  // (on the lyrics worker, debounced, into the cache entry).
  pill.SetLyricsOffsetHandler([&](const std::wstring& track_id, int offset_ms) {
    lyrics.SetTrackOffset(track_id, offset_ms);
  });
  bool lyrics_active = false;
  std::wstring lyr_pending_key;       // track + duration currently waiting out the debounce
  ULONGLONG lyr_pending_since = 0;
  std::wstring lyr_last_log;

  pill.SetCommandHandler([&](island::PillCommand c) {
    switch (c) {
      case island::PillCommand::Previous:  media.Previous(); break;
      case island::PillCommand::PlayPause: media.PlayPause(); break;
      case island::PillCommand::Next:      media.Next(); break;
    }
  });

  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  const bool in_is_console = in && in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode);

  std::wstring last_log_key;
  ULONGLONG next_settings_poll = GetTickCount64() + 1000;

  // Media + lyrics update work. Runs from the main loop AND from the pill's context-menu loop (the main loop is
  // blocked while the menu is open), so track changes and lyrics keep showing while the menu is up.
  auto service_updates = [&] {
    if (media_dirty.exchange(false)) {
      auto snap = media.Snapshot();
      pill.SetMedia(snap);

      // Only log real track / state changes.
      const std::wstring key = std::to_wstring(snap->has_session) + std::to_wstring(snap->is_playing) +
                               snap->title + L"\x1f" + snap->artist;
      if (key != last_log_key) {
        last_log_key = key;
        LogMedia(*snap);
      }
    }

    // ---- lyrics ----
    {
      const bool want = pill.LyricsWanted();
      if (want != lyrics_active) {
        lyrics_active = want;
        media.SetTimelineEnabled(want);   // timeline is read / subscribed only while lyrics mode is on
        if (!want) {
          lyrics.Cancel();
          lyr_pending_key.clear();
        }
        Log(want ? L"[lyrics] mode on\r\n" : L"[lyrics] mode off\r\n");
      }
      if (want) {
        auto snap = media.Snapshot();
        if (snap->has_session && !snap->title.empty() && snap->has_timeline) {
          const int dur = static_cast<int>((snap->duration_ms + 500) / 1000);
          const std::wstring id = island::TrackId(*snap);
          const std::wstring key = id + L"\x1e" + std::to_wstring(dur);
          const ULONGLONG t = GetTickCount64();
          if (key != lyr_pending_key) {
            lyr_pending_key = key;   // new track (or the player corrected the duration): let it settle first
            lyr_pending_since = t;
          } else if (t - lyr_pending_since >= 600) {   // skipping through songs must not fire one lookup per song
            island::LyricsRequest rq;
            rq.track_id = id;
            rq.title = snap->title;
            rq.artist = snap->artist;
            rq.album = snap->album;
            rq.duration_sec = dur;
            lyrics.Request(rq);   // no-op when this exact track + duration was already requested
          }
        }
      }
      if (lyrics_dirty.exchange(false)) {
        auto ls = lyrics.Snapshot();
        pill.SetLyrics(ls);
        const wchar_t* st = ls->state == island::LyricsState::Loading ? L"requested (local cache first, then LRCLIB)"
                          : ls->state == island::LyricsState::Ready ? L"found"
                          : ls->state == island::LyricsState::NotFound ? (ls->network_error ? L"network error" : L"none")
                          : L"idle";
        const std::wstring line = L"[lyrics] " + std::wstring(st) +
                                  (ls->state == island::LyricsState::Loading ? L" (" + std::to_wstring(ls->duration_sec) + L" s)"
                                                                             : std::wstring()) +
                                  (ls->state == island::LyricsState::Ready && ls->lines
                                       ? L" (" + std::to_wstring(ls->lines->size()) + L" lines)" : L"") + L"\r\n";
        if (line != lyr_last_log) { lyr_last_log = line; Log(line); }
      }
    }
  };
  pill.SetUpdateHandler(service_updates);

  bool running = true;
  while (running) {
    DWORD wait = MsgWaitForMultipleObjects(in_is_console ? 1 : 0, in_is_console ? &in : nullptr,
                                           FALSE, pill.NextWaitMs(), QS_ALLINPUT);
    if (in_is_console && wait == WAIT_OBJECT_0) {
      running = HandleConsoleInput(in, media, pill);
    }

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) running = false;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    service_updates();

    const ULONGLONG now = GetTickCount64();
    if (now >= next_settings_poll) {
      next_settings_poll = now + 1000;
      island::Settings fresh;
      if (watcher.Poll(&fresh)) {
        apply_cli(fresh);
        pill.ApplySettings(fresh);
        lyrics.SetCacheEnabled(fresh.lyrics_cache);
        media.SetPreferredApps(fresh.preferred_apps);
        media.SetSourceFilter(fresh.selected_only, fresh.selected_apps);
        Log(L"[settings reloaded]\r\n");
      }
    }

    pill.Tick();
  }

  lyrics.Stop();
  media.Stop();
  island::CloseSettingsWindow();
  tray.Destroy();
  pill.Destroy();
  if (single) CloseHandle(single);
  Log(L"Bye.\r\n");
  return 0;
}
