#pragma once
// Lyrics disk cache - the pure parts (no Win32, so tests/lyrics_test.cpp can build them with a host compiler):
// the cache FILE FORMAT and the "which files do we delete" decision. The actual file IO lives in lrclib.cpp.
//
// File format v2 (plain text header, then the raw LRC text):
//
//   island-lyrics-2\n
//   <key>\n                 normalised artist|title|duration (checked on read: a hash collision is a miss)
//   OK | NONE\n             lyrics found / "no lyrics" (negative cache entry)
//   <offset_ms>\n           per-track timing nudge (0 when never nudged)
//   <length>\n              exact byte length of the text that follows (a truncated file is rejected)
//   <LRC text, <length> bytes>
//
// v1 files (island-lyrics-1: header, key, status, then the LRC text) are still read; they are rewritten as v2
// the first time they are saved again.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace island {

// The per-track nudge is clamped to this many ms in either direction (the effective offset, global + track,
// is clamped to +-5000 by the UI).
constexpr int kMaxTrackOffsetMs = 10000;

struct CacheEntry {
  bool found = false;      // true: `lrc` holds synced lyrics; false: "no lyrics" (negative entry)
  int offset_ms = 0;       // per-track timing nudge; positive = lyrics appear later
  std::string lrc;         // UTF-8 LRC text (empty when !found)
};

// Serialises an entry as v2. Returns "" when the entry cannot be stored (a key with a line break in it).
std::string SerializeCacheEntry(const std::string& key, const CacheEntry& e);

// Parses a v1 or v2 file. False for anything wrong: bad magic, other key, bad numbers, truncated or
// over-long body, empty lyrics marked OK. `out` is only written on success.
bool ParseCacheEntry(const std::string& raw, const std::string& key, CacheEntry* out);

struct CacheFileInfo {
  std::wstring path;
  uint64_t size = 0;       // bytes
  uint64_t written = 0;    // last-write time in any consistent unit (FILETIME ticks); smaller = older
};

// Indices (into `files`) to delete, oldest first, so that at most `max_files` files and `max_bytes` bytes remain.
// Empty when the cache is already within both limits.
std::vector<size_t> PickEvictions(const std::vector<CacheFileInfo>& files, size_t max_files, uint64_t max_bytes);

} // namespace island
