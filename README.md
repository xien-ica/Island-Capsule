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

- Synced lyrics come from [LRCLIB](https://lrclib.net) (free, no account). Press `Ctrl+L` over the capsule to toggle them.
- No lyrics for a song? The capsule shows title + artist and switches back to lyrics on the next song that has them.
- Timing off? Hover and use `Ctrl+Left` / `Ctrl+Right` (remembered per song), or set a global **Lyrics offset** in Settings.
- Found lyrics are cached on disk (optional) so replays work offline. Settings → Behavior → Lyrics shows the cache size.
- Problems or missing lyrics? See [Limitations](#limitations).

## Settings

Right-click the capsule or tray icon → **Open settings**. Changes apply immediately.

- **Capsule** — position (left / center / right), margin, max width, control-open side, visualizer style (ticks / wave / glow / off). **React to audio** makes the visualizer follow real sound.
- **Behavior** — start with Windows, hide in fullscreen, collapse when paused, volume scroll & step size, Shift+scroll skipping, click actions, lyrics options, motion effects.
- **Media** — follow all media sessions, or only the executables you list (e.g. `Spotify.exe, chrome.exe`).

All options are also stored in the plain-text file  
`%APPDATA%\IslandCapsule\settings.ini`  
(you can edit it while the app is running).  
To disable every animation at once, set `reduced_motion=true`.

## Limitations

Lyrics come from the free, community-run [LRCLIB](https://lrclib.net) service, so they are only as complete and available as that service.

### Rate limiting (HTTP 429)
- LRCLIB limits how fast a client can ask. Skipping through many tracks quickly can trigger “429 Too Many Requests”.
- The capsule then pauses lookups (at least 5 s, doubling after each refusal up to 2 min, never shorter than the server’s `Retry-After`) and retries the current track up to 3 times by itself.
- If it still fails, it tries again the next time that track plays. Wait a moment or skip more slowly. Songs already cached are not affected.

### Server problems
- If LRCLIB is down, overloaded (5xx / 503) or times out, lyrics are unavailable until it recovers. The capsule keeps showing title + artist.
- A temporary outage is not remembered as “no lyrics”.

### Missing lyrics
- Not every song has synced lyrics. Newer, niche, regional and independent releases, remixes and live versions are often missing, or only have plain (un-timed) lyrics, which are not shown.
- Songs with no match are remembered as “no lyrics” for up to 14 days so they are not requested on every play.

### Matching and timing
- Lookups use the title, artist, album and duration your player reports. Odd tags (extra “feat.”, “Remastered”, translated titles) can cause no match or the wrong version.
- Synced lyrics are user-made, so some run slightly early or late. Use `Ctrl+Left` / `Ctrl+Right` or **Lyrics offset**.
- Players that do not report playback position cannot show lyrics at all.

### Offline and cache
- New songs need an internet connection; only cached songs work offline. Nothing is fetched while **Synced lyrics** is off.
- The disk cache holds up to 2000 files / ~16 MB; the oldest are removed first. Clearing the cache also clears your per-song timing nudges.

### Platform
- Island Capsule was only created and tested on Windows 11. It has not been tested on Windows 10, so it may or may not work there.

## Privacy

- The **only** network request is the lyrics lookup (title, artist, album, duration → lrclib.net). Requests also carry Island Capsule's public LRCLIB application ID (the `Lrclib-App-Id` header), which identifies the app, not you. With lyrics disabled there are zero network calls.
- The visualizer listens to system audio solely for animation. Nothing is recorded or stored. Turn off **React to audio** if you prefer.
- Local data lives in `%APPDATA%\IslandCapsule` (settings + optional lyrics cache). You can delete the `lyrics` folder at any time.

## Troubleshooting

- **Capsule doesn’t appear** — it only shows while media is actively playing. Start a track and wait a moment. If you restricted media sources, confirm your player is in the list.
- **Tray icon missing** — click the **^** overflow arrow next to the clock. You can drag the icon onto the taskbar.
- **Disappears in games / fullscreen video** — that’s the **Hide in fullscreen** option. Turn it off under Behavior if you want it always visible.
- **Player not detected** — some apps use unusual process names. Switch Media settings to “all apps”, or open an issue with the app name.
- **Lyrics don’t appear / “network error” / HTTP 429** — LRCLIB is rate limiting you or is busy; the capsule retries on its own (see [Limitations](#limitations)). Wait a few seconds and avoid skipping tracks rapidly. If a song never gets lyrics, it probably isn’t in LRCLIB yet.
- **Started twice** — a new instance automatically closes the previous one.
- **Quit** — right-click capsule or tray icon → **Quit**.
- **Uninstall** — quit, untick Start with Windows, then delete the program folder and `%APPDATA%\IslandCapsule`.

## Feedback

Bugs, ideas, or questions → [open an issue](https://github.com/xien-ica/Island-Capsule/issues).

## For developers

Build instructions, design notes and lyrics internals are in the [`docs/`](docs/) folder, starting with [`docs/development.md`](docs/development.md).

## License

[MIT](LICENSE)
