# Phase 2 – Capsule UI

- Autofit width (min/max), eased on track change.
- Marquee for any line wider than the max width: pause -> scroll -> seamless loop, with edge fade.
- Hover side-expand transport (prev / play-pause / next) as a second glass pill; the main
  pill never moves. Hover + press feedback on each button. Right anchor expands leftwards.
- The pill now receives mouse input; fully transparent pixels still click through to the taskbar.

Test:
1. Play a track with a very long title -> marquee scrolls after ~1.6 s.
2. Skip tracks -> width eases to the new size.
3. Hover the pill -> strip slides out; leave -> it collapses after ~0.25 s.
4. Click prev / play-pause / next -> the player responds; glyph switches play/pause.
5. Try `island_loader.exe right` -> the strip opens to the left.
