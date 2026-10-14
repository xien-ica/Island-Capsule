# Phase 3 – Album color system

- `theme/accent.*`  – dominant *vivid* color of the cover (24 hue bins, saturation/brightness weighted,
  neighbour smoothing). Grey / near-black covers fall back to a neutral, weaker tint.
- `theme/brushes.*` – `ComposeGlass()`: neutral glass base + low-alpha accent wash, art-side -> right-edge
  gradient, tinted rim, and adaptive text ink (luma of the fill over the taskbar decides light vs dark).
  The accent is luma-capped (dark taskbar) / floored (light taskbar) so text never lands on a muddy mid-grey.
- `ui/pill.cpp`     – extracts once per new cover, eases accent + strength (~0.4 s crossfade) on track change;
  keeps the old tint for 1.5 s if a player delivers art late; transport strip uses the same style.

Test:
1. Play a red / blue / green cover -> pill washes toward that hue; skip -> it crossfades.
2. Greyscale or black cover -> subtle neutral tint, text stays readable.
3. Switch Windows to light taskbar -> dark text, lighter wash.
4. Close the player -> tint fades back to neutral "No media".
