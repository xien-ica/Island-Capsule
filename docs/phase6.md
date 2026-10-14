# Phase 6 – Lyrics mode

**Ctrl+L while the cursor is over the capsule** toggles lyrics mode. The capsule then shows the current line of
the synced lyrics (from [LRCLIB](https://lrclib.net), no API key) instead of title / artist. Cover and visualizer stay.
Hovering still opens the transport controls, but in lyrics mode they open **above** the capsule.

## Behaviour

| | |
|---|---|
| Toggle | `Ctrl+L` over the capsule (or over its open strip). Saved as `lyrics_mode=true/false`, so it survives restarts. |
| Keyboard capture | A `WH_KEYBOARD_LL` hook is installed **only while the cursor is over the capsule** and removed the moment it leaves (same idea as the wheel hook). While installed it swallows only Ctrl+L (and that key's auto-repeat / key-up); Ctrl+Shift+L, Ctrl+Alt+L, Win+Ctrl+L and every other key pass through. Away from the capsule nothing is hooked, so the browser's Ctrl+L works normally. |
| Lyric line | Crossfades in over title / artist (0.3 s). Line changes slide up + fade (just a fade with `reduced_motion`). Long lines use the usual marquee. Intro / instrumental gaps show `♪`. |
| Width | Autofits to the current line, but never narrower than 70 % of `max_width` (so it doesn't breathe on every line) and never wider than `max_width`. |
| Up panel | Hover in lyrics mode: prev / play-pause / next rise out of the capsule's **top edge**. The panel is exactly as wide as the capsule (left/right edges aligned) and is clipped at the capsule's top line, so it slides up out of the capsule. The window gets taller for it (`win_h` grows by `up_extra_px`), the capsule itself does not move. Buttons are hit-tested in the panel (`TransportStrip::HitTestUp`). |
| No lyrics | Not found / instrumental / only unsynced lyrics / network error / the player reports no timeline: the normal capsule (title / artist, normal **side** strip) is shown **temporarily**. The wish for lyrics mode is kept; at the next song that has lyrics it switches back automatically. `Ctrl+L` again turns lyrics mode off completely (no auto-return). |
| While looking up | The normal capsule stays up; the lyric line fades in when the lyrics arrive. The lookup never runs on the UI thread. |
| Progress | **Nothing about progress is drawn.** The playback position is used internally to pick the line, nothing else. |

## Settings (`settings.ini`, Behavior tab → *Lyrics*)

| key | values | notes |
|---|---|---|
| lyrics_enabled | bool (default true) | master switch. Off => Ctrl+L does nothing, no hook, no lookups, no timeline reads |
| lyrics_mode | bool (default false) | what Ctrl+L toggles; written by the app |
| lyrics_offset_ms | -5000..5000 (default 0) | positive = lyrics appear **later**, negative = earlier. Applies to every song; a single song is nudged with Ctrl+Left / Ctrl+Right (below) |
| lyrics_cache | bool (default true) | keep fetched lyrics + per-song nudges on disk. Off => no disk reads / writes / cleanup (the 12-song memory cache still works, files already on disk are left alone) |

## Source files

- `src/lyrics/lrc.cpp/.hpp` – LRC parser (`ParseLrc`, `FindLyricLine`) + `LyricsSnapshot`. Several timestamps per line, `[offset:]`, enhanced-LRC `<mm:ss.xx>` tags, empty lines, `\r\n`, BOM.
- `src/lyrics/lyrics_text.cpp/.hpp` – pure helpers: UTF-8, URL-encoding, title / artist cleaning, a ~150-line JSON reader, ranking of search results.
- `src/lyrics/lyrics_cache.cpp/.hpp` – the disk-cache file format (v2, v1 still readable) and the eviction choice. Pure, host-tested.
- `src/lyrics/lrclib.cpp/.hpp` – `LyricsFetcher`: worker thread, disk cache IO, WinHTTP.
- `src/media/gsmtc.cpp` – timeline reading (`SetTimelineEnabled`, `MediaInfo::PositionNowMs`).
- `src/ui/pill.cpp`, `src/ui/transport.cpp` – lyric line drawing, Ctrl+L hook, up panel.
- `src/loader.cpp` – owns the fetcher, switches the timeline on/off, debounces lookups.
- `tests/lyrics_test.cpp` – host-side unit tests (106 checks) for everything in `src/lyrics` that has no Win32 in it (incl. the cache format and eviction).

## Lookup (`LyricsFetcher`)

1. Titles are cleaned first: `(Official Video)`, `[HD]`, `- Remastered 2011`, `feat. X`, `| Channel`, `- Topic`, `…VEVO`
   are removed; meaningful suffixes like `(Acoustic)`, `(Live)` or `- Radio Edit` stay.
2. Cache lookup (below). Miss =>
3. `GET /api/get?artist_name&track_name&album_name&duration` (no album on a 404 retry) →
   `GET /api/search?q=<artist title>` (best match: same title, matching artist, duration within 6 s, synced lyrics present) →
   same search with only the first artist (`A, B & C` → `A`) → YouTube-style `Artist - Title` split.
   At most 5 requests per song; a newer song cancels the running lookup at its next step.
4. `User-Agent: island-capsule/0.1 (https://github.com/YOUR-USER/island-capsule)` – **edit `kUserAgent` in `lrclib.cpp`**.
   WinHTTP timeouts: resolve 5 s, connect 5 s, send 5 s, receive 8 s.

**Cache** (`lyrics_cache`, on by default): `%APPDATA%\IslandCapsule\lyrics\<hash>.txt`, keyed by normalised artist +
title + duration in seconds. All file IO runs on the lyrics worker thread.
- "Found" is kept until evicted; "not found / instrumental" is cached too and retried after 14 days. A **network
  error is not cached** (it is retried the next time that song plays).
- **Duration tolerance**: on a miss the cache also looks for the same song at duration -1 s / +1 s (players round the
  length differently). Only found-lyrics entries count for the neighbours; "not found" is trusted for the exact key only.
- **File format v2**: magic, key, `OK|NONE`, per-song offset, byte length, then the LRC text. A truncated / padded /
  garbled file, a file for another key or a v-unknown file is simply a miss (the network result overwrites it). v1 files
  are still read.
- **Size cap**: 2000 files / 16 MB. A sweep runs on the worker after the first lookup of a session and then every 25
  writes: the oldest files (by last-write time) are deleted first, stale `.tmp` files from interrupted writes are removed.
  Only `*.txt` / `*.tmp` in the lyrics folder are touched. A cache hit on a found entry older than a day bumps its file
  time, so songs you keep playing outlive the ones you do not ("not found" entries are never bumped: their age is the
  14-day clock).
- Delete the folder to start over, or set `lyrics_cache=false` to stop using the disk.

## Timing nudge (per song)

**Ctrl+Left / Ctrl+Right while the cursor is over the capsule and a lyric line is showing**: lyrics 100 ms earlier /
later. Hold the key to repeat. **Ctrl+Down** resets this song's nudge. The capsule briefly shows `Lyrics +300 ms` (the
effective offset) in place of the line, then the line fades back.

- effective offset = global `lyrics_offset_ms` + this song's nudge, clamped to +-5000 ms. The keys only change the
  per-song part; the global value stays in the settings window (use it for player / Bluetooth latency).
- The nudge is stored **in the song's cache entry** (v2 `offset` field). It is saved by the lyrics worker ~0.5 s after the
  last key press (and at shutdown), never on the UI thread, and `settings.ini` is not touched. With `lyrics_cache=false` it
  is only remembered for the session (memory cache).
- Same keyboard-hook rules as Ctrl+L: the hook only exists while the cursor is over the capsule. Left / Right / Down are
  swallowed **only** with Ctrl alone **and** while lyrics are shown; Ctrl+Shift / Alt / Win + arrows, and Ctrl+arrows when
  no lyric is showing, pass straight through.

## Timing

`MediaSession::SetTimelineEnabled(true)` (only while lyrics mode is wanted) makes the GSMTC worker read Position,
StartTime / EndTime, LastUpdatedTime and PlaybackRate and subscribe to `TimelinePropertiesChanged`. The position is
brought up to "now" using `LastUpdatedTime`, then the UI extrapolates between updates while playing
(`MediaInfo::PositionNowMs`). A timeline-only event does not re-read the cover or re-layout the capsule.
With lyrics mode off the timeline is never read and there is no timeline subscription (no extra wakeups).

## What is NOT tested (written without a Windows machine)

- **Everything on Windows.** Only syntax-checked with llvm-mingw (clang, `-std=c++20 -Wall`), plus the host unit tests.
- **`gsmtc.cpp` was not even syntax-checked** (the C++/WinRT headers are generated by the Windows SDK and weren't available): the
  timeline code uses `GetTimelineProperties()`, `TimelinePropertiesChanged`, `PlaybackInfo().PlaybackRate()` and
  `winrt::clock::now() - LastUpdatedTime()`. If MSVC complains, it will be there.
- **The real LRCLIB API** (the build sandbox could not reach lrclib.net): URL building, WinHTTP calls and the JSON field names
  (`trackName`, `artistName`, `albumName`, `duration`, `syncedLyrics`, `instrumental`) follow the documented API but were never run against it.
- **Players:** whether Spotify / Chrome / Edge / foobar report a usable Position + LastUpdatedTime. A player that reports no
  timeline gets no lyrics (default capsule). A player that reports a stale position gets lyrics that drift; use `lyrics_offset_ms`.
- **Look and feel:** the up panel's corners (its flat, clipped bottom edge is as wide as the capsule, so a small transparent notch
  shows beside the capsule's rounded top corners), the 0.3 s timings, the min width rule, the `♪` placeholder.
- **Keyboard hook:** swallowing Ctrl+L and its key-up while hovering, and that the hook is really gone once the cursor leaves.
- **Cache + nudge (added later):** none of it ran on Windows. Host-tested: the v2/v1 file format (corrupt, truncated, wrong-key
  files) and the eviction picker. Only syntax-checked (against stub headers): the file IO, sweep, touch and nudge saving in
  `lrclib.cpp`. Not even that: the Ctrl+Arrow hook code, the indicator drawing and the settings row.

## How to test (in the order they were built)

1. `cmake -DISLAND_TESTS=ON ..` then build `lyrics_test` and run it: 73 checks, 0 failed.
2. Run the loader with the console on, press **Ctrl+L over the capsule**: `[lyrics] Ctrl+L: lyrics mode on`, then
   `[lyrics] mode on`, `[lyrics] looking up`, `[lyrics] found (N lines)` (or `none` / `network error`). `settings.ini` now has `lyrics_mode=true`.
3. Play a song with lyrics: the capsule fades from title / artist to the current line and the line follows the music. Seek,
   pause, skip: the line follows. Check `%APPDATA%\IslandCapsule\lyrics\` for the cache file; replay the song, no network needed.
4. Skip to a song without lyrics: the normal capsule + side strip; skip to one with lyrics: lyrics come back by themselves.
5. Hover in lyrics mode: the controls rise above the capsule; click prev / play-pause / next. Hover with no lyrics: strip opens sideways.
6. In a browser, press Ctrl+L with the cursor **away** from the capsule (address bar focuses) and **over** it (lyrics toggle, address bar does not).
7. Adjust *Lyrics offset* in the settings window while a song plays.
7b. Nudge: hover the capsule with lyrics showing, press Ctrl+Right / Ctrl+Left (hold to repeat): `Lyrics +100 ms`, `+200 ms`...
   shows up and the line timing moves; Ctrl+Down resets. Restart the app and replay the song: the nudge is back (check the
   `offset` line, 4th line, in the cache file). Ctrl+Left in a text box (cursor away from the capsule) still jumps words.
7c. Cache: with `lyrics_cache=false` nothing new appears in the lyrics folder; back on, it fills again. To see eviction,
   temporarily lower `kCacheMaxFiles` in `lrclib.cpp` (e.g. 5) and play 6+ songs.
8. Close and restart the loader with `lyrics_mode=true`: lyrics mode comes back with the first song that has lyrics.

## Performance notes

- **Fetcher:** one WinHTTP session / TLS connection is kept for every lookup (previously re-opened per song, including
  proxy auto-detection), with gzip + HTTP/2 enabled. A 12-entry in-memory cache of finished lookups makes revisiting a
  song instant. The disk-cache write happens *after* the lyrics were published to the UI.
- **Parser:** `ParseLrc` works on string views (no per-line copies) and only sorts when the input is out of order.
- **Line tracking:** `FindLyricLineHinted` is O(1) while a song plays on (binary-search fallback after seeks).
- **Drawing:** line widths are measured on one persistent off-screen surface and cached per text; the loop wakes
  exactly when the next line is due instead of up to one 50 ms tick late.
- **Render target:** the DIB section + memory DC are created once and reused (with spare capacity, so the width
  animation doesn't reallocate); previously they were created and destroyed every frame. The redundant `memset`
  before `g.Clear()` was removed. `UpdateLayeredWindow` no longer needs a screen DC.

### Performance pass 2 (idle cost + per-frame allocations)

- **Environment cache (`pill.cpp`, `Tick`)**: the taskbar `HWND` is found once (re-found only if `IsWindow` fails, i.e. explorer
  restarted). The DPI is re-read on `WM_SETTINGCHANGE` / `WM_DISPLAYCHANGE` / `WM_DPICHANGED` / `WM_THEMECHANGED`
  (`env_dirty`) or once per `kEnvPoll` (1 s) as a safety net, instead of FindWindow + DPI + a registry query on every tick.
- **Theme**: the capsule glass is always the dark variant (`dark` is constant `true`); the Windows light/dark setting is no
  longer read. The album tint alone decides the look.
- **Fullscreen**: `TaskbarCover()` looks at the *foreground window*: not in the taskbar's (explorer's) process, visible, not
  minimized, on the taskbar's monitor, covering the whole monitor, and not an ordinary captioned maximized window => covered.
  A hidden / off-screen (auto-hide) taskbar also counts. It runs every tick (a few cheap calls), has no debounce, hides instantly
  and re-shows at once (fading in from 45 %). `NULL` foreground (mid-switch) keeps the current state. Turn it off with
  `hide_fullscreen=false`.
- **Idle loop**: `NextWaitMs()` idles at `kIdleWaitMs` (50 ms). Lyric-line wake-ups stay exact.
- **Z-order guard (`GuardZ`)**: Windows re-raises the taskbar over the capsule when it shows a taskbar thumbnail / jump list or when a
  taskbar button is clicked. While the cursor is over the taskbar or the `kHotAboveDp` strip above it (plus `kHotHold` after leaving),
  the loop wakes every `kGuardWaitMs` (4 ms) and only checks `TaskbarAbove` + re-asserts topmost; the full update/render still runs at
  its normal pace (`PillWindow::Tick` throttles it). Outside that area nothing changes.
- **`ComposeGlass`** runs only when the (eased) accent / strength changed.
- **Cached GDI+ objects**: capsule body/rim paths + gradient brush + rim pen (`SurfaceCache`), cover clip path / placeholder brush /
  outline pen (`ArtGfxCache`), the cover `TextureBrush` while no crossfade runs (`ScaledArt::brush`), the marquee off-screen
  bitmap (`MarqueeScratch`), and the visualizer's tick path + brush. Animated colours are updated in place.
  `ReleaseDrawCaches()` frees them (and the scaled covers) before `GdiplusShutdown`.
