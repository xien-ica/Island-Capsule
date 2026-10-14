# Phase 4 – Mini visualizer

`ui/visualizer.*` – the signature element, in a fixed 28x18 (logical px) slot on the **right inside the pill**.
The text area reserves the slot, so the pill autofits around it and the marquee never runs underneath.

**Reactivity (no audio capture):** per band = two sines + smooth value-noise + a beat pulse
(bass leans on the beat). Tempo, phases and noise offset are seeded from a hash of title+artist, so each
track has its own feel. Fast attack / slow release smoothing makes it read like an analyser.
`Visualizer::SetBands()` is the hook for a future WASAPI/FFT source - `Draw()` doesn't change.

**States**
- playing  -> bands animate; spring-up from paused with a small overshoot
- paused   -> "living pause": collapses to two dots, springs back on play
- no media -> two dots, dimmed

**Styles** (`visualizer=` in settings.ini or `--viz=`): `ticks` (6 rounded bars, default), `wave` (smooth ribbon),
`glow` (accent bloom that breathes with the bass), `off` (slot freed, pill gets narrower).

**Accent-linked:** color comes from `GlassStyle::viz` (album accent pushed to read on the glass; neutral ink when
there is no tint). It eases with the same ~0.4 s accent crossfade, so bars recolor on track change.

**Performance:** the visualizer redraws at ~30 fps (`kVizFrame`); any other animation runs at ~16 ms.
Idle / paused-and-settled drops to a 250 ms poll with no redraws. Only the pill's own small layered bitmap is
re-rendered - nothing touches the rest of the taskbar.

Test:
1. Play music -> bars move; skip -> motion character changes slightly.
2. Pause -> two dots; play -> bars spring back.
3. Different covers -> bars take the album hue. Light taskbar -> deeper bars.
4. `island_loader.exe --viz=wave` / `--viz=glow` / `--viz=off`.
