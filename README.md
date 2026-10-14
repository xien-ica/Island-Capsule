# Island Capsule

A small Dynamic Island–style media capsule for the **Windows 11 taskbar**.

> Status: This project is still in active development. Expect changes, improvements, and occasional rough edges while features are being refined.

It shows what’s currently playing, lets you control playback with hover or scroll, and can display **synced lyrics** right on the taskbar. It follows any media that appears in Windows’ System Media Transport Controls (Spotify, browsers, and most other media apps).

> **Inspiration**  
> The idea of putting a compact “island” media control on the Windows 11 taskbar comes from the excellent Windhawk mod [Island Media Controls](https://windhawk.net/mods/island-media-controls) by *[u/...]*.  
> Island Capsule is a completely separate, from-scratch standalone application. It is **not** a Windhawk mod, does not share code with that project, and is not affiliated with it. If you prefer the native-taskbar integration approach, go try the original mod — it’s great.

## Features

- **Now playing** — album art, title and artist, tinted by the album’s colors. Long titles scroll.
- **Live visualizer** — reacts to the real audio coming from your speakers (or a simple animation if you prefer).
- **Hover controls** — previous / play-pause / next slide out of the capsule.
- **Scroll for volume** — mouse wheel over the capsule changes system volume.
- **Synced lyrics** — `Ctrl+L` over the capsule switches to the current lyric line (sourced from [LRCLIB](https://lrclib.net)).
- **Stays out of the way** — collapses when paused, hides when nothing is playing, and can hide during fullscreen apps/games.

## Get started

1. Download the latest release from the [Releases page](https://github.com/xien-ica/Island-Capsule/releases).
2. Run `island_loader.exe`. Play something — the capsule appears on the taskbar after a second or two.
3. To start with Windows: right-click the tray icon (it may be under the **^** arrow next to the clock) → **Start with Windows**.

Requires **Windows 11**.  
If you move the program later, untick and re-tick **Start with Windows** so it remembers the new path.

## Controls

| Action over the capsule | Result |
|---|---|
| Hover | Previous / play-pause / next controls appear (above the capsule when lyrics are showing) |
| Mouse wheel | System volume (a temporary volume bar replaces the title) |
| Shift + wheel | Skip tracks (down = next, up = previous) |
| Middle-click | Play / pause |
| Right-click | Menu: copy title + artist, lyrics mode, settings, quit |
| `Ctrl+L` | Toggle lyrics mode |
| `Ctrl+Left` / `Ctrl+Right` | Nudge lyrics timing −0.1 s / +0.1 s for the current song (hold to repeat) |
| `Ctrl+Down` | Reset timing offset for the current song |

Keyboard shortcuts only work while the mouse is over the capsule, so they never conflict with normal shortcuts in other apps. Timing keys only respond while a lyric line is visible.

## Lyrics

- Lyrics are fetched from [LRCLIB](https://lrclib.net) (free, no account required).
- Toggle with `Ctrl+L`. The capsule follows the music through pause, seek and skip.
- No lyrics available? It falls back to title + artist for that song and automatically shows lyrics again on the next track that has them.
- Timing a bit early/late? Hover and use `Ctrl+Left` / `Ctrl+Right`. The offset is remembered per song. For a global offset (e.g. Bluetooth latency) use **Lyrics offset** in Settings.
- Found lyrics are cached locally (optional; can be turned off). Cache is capped at ~16 MB.
- Some players do not report playback position — lyrics cannot be shown for those sessions.

## Settings

Right-click the capsule or tray icon → **Open settings**. Changes apply immediately.

- **Capsule** — position (left / center / right), margin, max width, control-open side, visualizer style (ticks / wave / glow / off). **React to audio** makes the visualizer follow real sound.
- **Behavior** — start with Windows, hide in fullscreen, collapse when paused, volume scroll & step size, Shift+scroll skipping, click actions, lyrics options, motion effects.
- **Media** — follow all media sessions, or only the executables you list (e.g. `Spotify.exe, chrome.exe`).

All options are also stored in the plain-text file  
`%APPDATA%\IslandCapsule\settings.ini`  
(you can edit it while the app is running).  
To disable every animation at once, set `reduced_motion=true`.

## Privacy

- The **only** network request is the lyrics lookup (title, artist, album, duration → lrclib.net). With lyrics disabled there are zero network calls.
- The visualizer listens to system audio solely for animation. Nothing is recorded or stored. Turn off **React to audio** if you prefer.
- Local data lives in `%APPDATA%\IslandCapsule` (settings + optional lyrics cache). You can delete the `lyrics` folder at any time.

## Troubleshooting

- **Capsule doesn’t appear** — it only shows while media is actively playing. Start a track and wait a moment. If you restricted media sources, confirm your player is in the list.
- **Tray icon missing** — click the **^** overflow arrow next to the clock. You can drag the icon onto the taskbar.
- **Disappears in games / fullscreen video** — that’s the **Hide in fullscreen** option. Turn it off under Behavior if you want it always visible.
- **Player not detected** — some apps use unusual process names. Switch Media settings to “all apps”, or open an issue with the app name.
- **Started twice** — a new instance automatically closes the previous one.
- **Quit** — right-click capsule or tray icon → **Quit**.
- **Uninstall** — quit, untick Start with Windows, then delete the program folder and `%APPDATA%\IslandCapsule`.

## Feedback

Bugs, ideas, or questions → [open an issue](https://github.com/xien-ica/Island-Capsule/issues).

## For developers

Build instructions, design notes and lyrics internals are in the [`docs/`](docs/) folder, starting with [`docs/development.md`](docs/development.md).
