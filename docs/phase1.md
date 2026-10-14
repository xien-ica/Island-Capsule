# Phase 1 – Media core (GSMTC)

Done:
- `src/media/gsmtc.*` – session manager, optional preferred-app filter,
  title / artist / album / play state / can-skip / can-prev, thumbnail
  (decoded to a 96x96 square, premultiplied BGRA), transport commands.
- One background worker thread does all WinRT work; the UI thread only reads
  an immutable snapshot and queues commands, so it never blocks.
- `src/ui/pill.*` – minimal readout (art + title + artist, autofit width,
  ellipsis) so the media data is visible. Marquee / hover transport / album tint /
  visualizer are Phases 2-4.

Run:

    island_loader.exe                       # centered
    island_loader.exe left                  # left | center | right
    island_loader.exe --prefer=spotify,chrome

Console keys: Space = play/pause, Left/Right = prev/next, Enter = quit.

Test checklist:
1. Start music in Spotify / browser / any SMTC player -> pill shows art, title, artist.
2. Skip tracks in the player -> pill updates (art may arrive ~1 s later).
3. Pause/play -> console shows `[paused]` / `[playing]`.
4. Space / arrows in the console control the player.
5. Close the player -> pill shows "No media".
