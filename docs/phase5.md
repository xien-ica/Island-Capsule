# Phase 5 – Polish + settings

**Settings** – `%APPDATA%\IslandCapsule\settings.ini` (created with commented defaults on first run, hot-reloaded
every ~1 s; command-line options override the file):

| key | values | notes |
|---|---|---|
| position | left / center / right | |
| margin | 0..200 | logical px |
| max_width | 200..600 | widest the pill may grow |
| visualizer | ticks / wave / glow / off | |
| seam_glow | bool | accent glow crawls out of the join when the strip opens |
| hide_fullscreen | bool | fade out while a fullscreen app / game / video is up (default true) |
| scroll_volume | bool | mouse wheel over the capsule changes system volume (default true) |
| scroll_step | 1..10 | percent per wheel notch (default 2) |
| strip_side | left / right | side the transport strip opens on (default right) |
| collapse_paused | bool | paused => capsule collapses to just the visualizer; hover reopens it |
| overshoot | bool | springy bounce when the strip / controls open |
| hover_scale | bool | capsule grows slightly on hover |
| text_slide | bool | text slides on track / lyric changes |
| reduced_motion | bool | master switch: turns all of the above and seam glow off |
| preferred_apps | list | prefer these apps, fall back to Windows' pick (hot-reloaded) |
| media_source | all / selected | `selected` = ONLY the apps in `selected_apps` are used |
| selected_apps | list | e.g. `Spotify.exe, chrome.exe`; separators: comma, semicolon, slash, pipe, whitespace; `.exe` optional |

CLI: `left|center|right`, `--prefer=a,b`, `--viz=...`, `--reduced-motion`, `--autostart` (set by "Start with Windows": drops the console).

**Motion** (`ui/motion.hpp` springs)
- Elastic strip: spring with ~3 % overshoot; the strip slides out from under the main pill (see polish).
- Pop in/out: startup and idle-hide fade (no scale).
- Cover crossfade (0.35 s). A cover that arrives late keeps the old one for 1.5 s instead of flashing a placeholder.
- Skip-direction text slide: Next slides the old text out left / new in from the right, Prev mirrors it.
  Direction comes from the strip buttons and the console arrow keys (`PillWindow::NotifySkip`); track changes with no
  skip (auto-advance) crossfade without travel.

**Performance**
- Frame pacing is explicit: `PillWindow::NextWaitMs()` -> 16 ms in motion, 33 ms visualizer-only, 250 ms idle.
- Mouse-move floods and timeline events cannot over-render: visualizer-only redraws are capped to ~30 fps.
- Z-order re-assert is twice a second instead of every tick; console logging only on real track/state changes.

Test:
1. Hover -> the strip capsule springs out from under the main pill with a tiny overshoot and a glow crawling from the join; leave -> collapses.
2. Click Next / Prev -> text and cover slide/crossfade in the matching direction.
3. Edit settings.ini while running (e.g. `visualizer=wave`) -> applies within ~1 s.
4. Close the player -> the capsule fades out after ~1.5 s (no "No media" state); start playing -> it pops back.
5. `reduced_motion=true` -> no overshoot / scale / slide.

## Polish pass (after phase 5)

- Removed the progress underline (setting, CLI flag, timeline reading and timeline wakeups) and all scaling
  (no hover grow, no pop-in scale, no transparent window margin).
- Side expansion is a separate, slightly shorter capsule (1 px inset top/bottom) drawn *under* the main pill.
  Its inner end is tucked beneath the pill, glass is never blended over glass (the pill area is excluded
  when the strip is drawn) and a soft contact shadow sits on the pill edge. Seam glow is back.
- Cover: left padding 4 -> 7 px so it clears the round end. Decoded up to 256 px (was 96), area-average
  downsampled once to the exact on-screen size, drawn with an anti-aliased rounded fill (not a jagged clip).
- Text: whole-pixel font sizes (13 / 11 px), whole-pixel text origins and marquee offsets.

## Polish pass 2

- Hover: the strip opens on `strip_side` (right by default) and the *group* (strip + main capsule) stays anchored,
  so the main capsule slides to the opposite side while the pair stays centered in the taskbar.
- Hover scale is back (3 %) as one transform over the whole group, so text and cover scale with the capsule
  instead of having their own scaling. 8 px transparent window margin gives it room.
- Paused (after 0.6 s, to ignore track-change gaps): the capsule collapses toward its middle to a small capsule
  holding only the visualizer (its paused dots); art/text fade out. Playing again opens it back out from the
  middle. Hovering a collapsed capsule reopens it so the controls are reachable. Needs the visualizer on.
- Cover margin: left padding 10 px, cover 26 px.

## Polish pass 3

- Strip defaults to the right (`strip_side=right`).
- Strip is 1 px shorter than the main capsule (was 3 px) and roomier: 5 px edge padding and 5 px between
  buttons (was 3 / 2), so the buttons are no longer cramped. It tucks 60 % of the pill height under the pill.

## Polish pass 4 - tray, fullscreen, scroll volume

- **Tray icon** (`ui/tray.cpp`): right-click -> Open settings / Start with Windows (checkmark) / Quit. The icon is
  drawn in code. "Start with Windows" writes `HKCU\...\Run\IslandCapsule = "<exe>" --autostart`; it follows the
  exe path, so toggle it again after moving the exe. Survives an explorer restart (TaskbarCreated).
- **Hide in fullscreen** (`hide_fullscreen`): polled twice a second with `SHQueryUserNotificationState`
  (fullscreen / D3D fullscreen / presentation) plus a fallback for borderless fullscreen windows (visible,
  caption-less foreground window covering its monitor). Hides instantly when a fullscreen app appears and fades back in when it ends.
- **Scroll wheel volume** (`scroll_volume`, `scroll_step`): `audio/volume.cpp` (Core Audio, default playback
  device). Raising the volume also unmutes. While scrolling, the title/artist give way to a volume HUD
  (speaker glyph, bar, percent) that fades back after ~1.4 s.
- CMake option `ISLAND_CONSOLE` (default ON). `-DISLAND_CONSOLE=OFF` builds without a console window.

## Fix pass (after polish 4)

- Restored the side-capsule padding (inner 6 px next to the main capsule, outer 12 px at the far end) that
  the previous zip had overwritten.
- Scroll volume now uses a low-level mouse hook while the cursor is over the capsule (the plain WM_MOUSEWHEEL
  route only works when Windows delivers wheel events to inactive windows). The wheel is swallowed there so the
  app underneath does not scroll too.
- Single instance: a new copy asks the old one (through its tray window) to quit, so rebuilds never leave two
  capsules stacked on each other. Older builds without a tray icon must be closed in Task Manager once.
- Console diagnostics: `[tray]`, `[fullscreen] hiding / showing again (shell state N)`, `[volume] N%`.

## Fix pass 2 - app filter, hide with no media

- `media_source=selected` + `selected_apps=` filter which media apps the capsule follows (same idea as the
  visualizer's "Audio source: Selected applications"). Matching is a case-insensitive substring of the session's
  app id with `.exe` dropped, so `Spotify.exe`, `chrome.exe`, `msedge.exe` work. The console log prints each
  session's app id in braces (`{...}`): copy that if an app is not matched (e.g. Firefox uses a hashed id).
  Order of preference when several selected apps play: Windows' current session, then a playing one, then the
  first match. Hot-reloaded: no restart. `preferred_apps` is hot-reloaded now too.
- No media (nothing playing, or everything filtered out) now always hides the capsule after ~1.5 s. The old
  `hide_when_idle` setting and `--hide-idle` flag are gone; leftover lines in settings.ini are ignored.

## Settings window

Tray -> **Open settings** now opens a small native window instead of Notepad (`ui/settings_window.cpp`).

- Three tabs: **Capsule** (position, margin, max width, strip side, visualizer), **Behavior** (start with Windows,
  hide in fullscreen, collapse when paused, scroll volume + step, seam glow, reduced motion), **Media**
  (all / selected apps, selected + preferred app lists).
- Every change is written straight to `settings.ini` (only that key, comments are kept) and the loader
  hot-reloads it, so the capsule updates within ~1 s. App lists are saved ~0.5 s after you stop typing.
- Follows the Windows light / dark theme and per-monitor DPI. `Esc` closes it.
- "Open settings.ini" at the bottom still opens the raw file for anything advanced.
- Opening the window again just brings the existing one to the front.

## Fullscreen detection fix (capsule blinking when switching apps)

- **Debounce**: the capsule now hides only after fullscreen has held for 0.6 s and comes back after it has been gone
  for 0.4 s (`kFsEnterDelay` / `kFsExitDelay` in `pill.cpp`). Alt-Tab, Task View or a window opening can look like a
  fullscreen window for a moment; that no longer makes the capsule blink. Polling is 4x/second.
- **Shell windows ignored**: Alt-Tab / Task View / staging windows (`MultitaskingViewFrame`, `TaskSwitcherWnd`,
  `ForegroundStaging`, ...) are never treated as fullscreen apps.
- **"Busy" state verified**: the shell's busy state is only trusted when the foreground window really covers the monitor.
- **Same monitor only**: a fullscreen app on another monitor no longer hides a capsule that lives on a different one.
- **Own windows ignored** (capsule, tray, settings window), and turning `hide_fullscreen` off brings the capsule back at once.
- Console log lines `[fullscreen] hiding / showing again (shell state N)` are unchanged; if it still blinks, send those lines.

## Z-order fix (capsule disappearing for a moment when switching apps)

The taskbar re-raises itself above the capsule when the foreground app changes; the capsule only re-asserted its
z-order twice a second, so it stayed hidden behind the taskbar until the next check. A `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)`
now triggers a short burst (0.8 s) of per-frame re-asserts (`z_burst` in `pill.cpp`), so it pops back above the taskbar
immediately. The hook is removed in `PillWindow::Destroy()`.

## Taskbar-following visibility (replaces the fullscreen heuristics)

The capsule no longer tries to *guess* what a fullscreen app is (shell state, window classes, rect checks).
It follows the taskbar instead (`TaskbarCovered` in `pill.cpp`):

- Every ~50 ms (16 ms right after an app switch) it asks `WindowFromPoint` who is on top at a point on the taskbar
  just outside the capsule's window. Taskbar on top = show; any other window on top, taskbar hidden, or an
  auto-hidden taskbar slid off screen = hide. Whatever makes the taskbar disappear makes the capsule disappear.
- Hide is instant (no fade); a 0.10 s / 0.15 s debounce (`kFsEnterDelay` / `kFsExitDelay`) filters one-frame glitches.
- The z-order check is now exact too: `TaskbarAbove` sees when the taskbar was raised over the capsule and the
  capsule re-asserts itself immediately (plus the foreground-change burst).
- Console: `[taskbar] covered by <window class>` / `[taskbar] visible again`. The class name shows what covered it.
- `hide_fullscreen=false` still means "always show".
