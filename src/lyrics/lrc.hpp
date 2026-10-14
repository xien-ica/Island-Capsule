#pragma once
// Lyrics – plain data + LRC parser. No Win32 / WinRT types in here, so the UI and the tests
// (tests/lyrics_test.cpp, built with a host compiler) can include it freely.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace island {

struct LyricLine {
  int64_t ms = 0;        // timestamp of the line, milliseconds from the start of the track
  std::wstring text;     // may be empty: an instrumental gap
};

// Parses synced lyrics ("[mm:ss.xx] text"). Handles several timestamps per line
// ("[00:10.00][01:20.00]chorus"), [offset:] tags, enhanced-LRC word tags (<mm:ss.xx>), \r\n, empty lines.
// The result is sorted by time. Consecutive empty lines are merged, a leading empty line is dropped.
std::vector<LyricLine> ParseLrc(const std::wstring& text);

// Index of the line that is current at `pos_ms` (the last line whose time is <= pos_ms),
// or -1 while the position is before the first line (or the list is empty).
int FindLyricLine(const std::vector<LyricLine>& lines, int64_t pos_ms);

// Same answer as FindLyricLine, but O(1) when `hint` (the previous answer) is still current or the next
// line has just started - the normal case while a song plays. Falls back to the binary search after seeks.
int FindLyricLineHinted(const std::vector<LyricLine>& lines, int64_t pos_ms, int hint);

enum class LyricsState {
  Idle,       // nothing requested (lyrics mode off)
  Loading,    // lookup running
  Ready,      // `lines` holds synced lyrics for `track_id`
  NotFound    // no (synced) lyrics: not found, instrumental or a network error
};

struct LyricsSnapshot {
  std::wstring track_id;    // identity of the track this belongs to (see TrackId in media_info.hpp)
  int duration_sec = 0;
  LyricsState state = LyricsState::Idle;
  bool network_error = false;   // NotFound because the lookup failed (not cached)
  int track_offset_ms = 0;      // per-track timing nudge stored in the cache entry (Ready only); positive = later
  std::shared_ptr<const std::vector<LyricLine>> lines;
};

} // namespace island
