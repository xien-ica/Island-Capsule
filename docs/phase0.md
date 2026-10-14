# Phase 0 – Shell

Goal: empty tinted pill sitting on the primary taskbar.

## Steps

1. **Find the taskbar window**
   - `FindWindowW(L"Shell_TrayWnd", nullptr)` for the primary bar.
   - Handle multi-monitor later if needed.

2. **Obtain the XAML root**
   - Existing Windhawk mods use interop helpers to reach the `FrameworkElement` root of the taskbar.
   - Common pattern: locate the XAML Island / DesktopWindowXamlSource associated with the taskbar.

3. **Create a host element**
   - A simple `Border` or `Grid` with:
     - Corner radius ≈ 50 % of height
     - Semi-transparent fill (`#202024` @ ~60 % dark / `#F4F4F6` @ ~40 % light)
     - Soft 1 px rim
   - Fixed height matching taskbar (probe actual height + DPI).

4. **Insert into the layout**
   - Preferred: center overlay or reserved slot so it doesn’t cover icons permanently.
   - Start with absolute positioning overlay for fastest feedback; refine to proper Grid column later.

5. **DPI + theme probe**
   - Watch `WM_DPICHANGED` / display changes.
   - Detect dark/light via system theme or taskbar acrylic sample.

6. **Settings stub**
   - Position (left / center / right), margins, hide-when-idle flag.

## Success criteria for Phase 0

- Empty glass pill appears on the primary taskbar.
- Survives explorer restart (or re-attaches cleanly).
- Correct size for current DPI / taskbar height.
- No crashes, no UI thread blocks.

## Next

Once the empty pill is stable → Phase 1 (GSMTC).
