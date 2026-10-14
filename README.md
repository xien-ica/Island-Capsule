# Island Capsule

Compact Dynamic Island–style media capsule for the Windows 11 taskbar.

**Signature features**
- Mini album art + title/artist (autofit + marquee)
- Right-side mini visualizer (always visible in the pill)
- Glass surface tinted by album art
- Tray icon, start with Windows, hide in fullscreen, scroll-wheel volume
- Side-expand transport controls (hover) - a second, shorter capsule slides out from under the main pill
- Lyrics mode: Ctrl+L over the capsule shows the current synced lyric line (LRCLIB); controls then open above the capsule
- Clean multi-file architecture (~4–8k lines target)

This is a **from-scratch** recreate, not a strip of the ~18k Island Media Controls mod.

## Build order (phases)

| Phase | Focus |
|-------|--------|
| 0 | Shell: inject into primary taskbar XAML root, empty tinted pill |
| 1 | Media core: GSMTC session, title/artist/art, play state, transport commands |
| 2 | Capsule UI: pill, art, dual-line text, autofit, marquee, side-expand |
| 3 | Album color system: dominant/accent, tint, adaptive text |
| 4 | Mini visualizer (time-based first) - done, see `docs/phase4.md` |
| 5 | Polish + settings - done, see `docs/phase5.md` (incl. polish pass) |
| 6 | Lyrics mode (Ctrl+L, LRCLIB synced lyrics, transport panel above the capsule) - see `docs/phase6.md` |

## Platform choice

Prefer **standalone multi-file DLL + loader** for clean architecture.  
Windhawk can be used later as a thin loader if easy distribution is needed.

## Requirements

- Windows 11 (primary target)
- Visual Studio 2022 Build Tools or full VS (C++ desktop + Windows SDK)
- CMake ≥ 3.20
- Windows 10/11 SDK (for WinRT / XAML interop)
- VS Code + C/C++ extension + CMake Tools (recommended)

## Project layout

```
island-capsule/
  src/
    inject/          # taskbar attach / XAML injection
    media/           # GSMTC session manager
    ui/
      capsule.cpp    # layout, autofit, marquee
      transport.cpp  # side expand + buttons
      visualizer.cpp # right-side mini viz
    theme/
      accent.cpp     # color from art
      brushes.cpp    # glass fills
    settings/
    lyrics/          # LRCLIB client, LRC parser, text helpers
  include/
  docs/
  assets/
```

## Getting started (VS Code)

1. Open the `island-capsule` folder in VS Code.
2. Install recommended extensions (C/C++, CMake Tools).
3. Configure the kit for MSVC + Windows SDK.
4. Build the empty Phase 0 shell first.

See `docs/phase0.md` for the first concrete steps.
