#pragma once
// Lyrics – LRCLIB client (https://lrclib.net, no API key).
//
// Threading: everything (disk cache, WinHTTP, JSON, LRC parsing) runs on ONE background worker
// thread. The UI thread only calls Request()/Cancel()/Snapshot(), which never block on the network.
// on_changed is invoked on arbitrary threads: post a message, don't touch UI objects from it.

#include <functional>
#include <memory>
#include <string>

#include "lyrics/lrc.hpp"

namespace island {

struct LyricsRequest {
  std::wstring track_id;     // opaque identity of the track (TrackId() in media_info.hpp); echoed in the snapshot
  std::wstring title, artist, album;
  int duration_sec = 0;      // 0 = unknown
};

class LyricsFetcher {
public:
  using ChangedFn = std::function<void()>;

  LyricsFetcher();
  ~LyricsFetcher();
  LyricsFetcher(const LyricsFetcher&) = delete;
  LyricsFetcher& operator=(const LyricsFetcher&) = delete;

  bool Start(ChangedFn on_changed);
  void Stop();

  // Looks the track up (cache first, then LRCLIB). The snapshot switches to Loading at once and to
  // Ready / NotFound when the worker is done. A newer Request() supersedes a running one; asking for the
  // same track_id + duration again is a no-op.
  void Request(const LyricsRequest& req);
  // Lyrics mode is off: drop the current result and abort a running lookup at its next step.
  void Cancel();

  // Disk cache on / off (setting lyrics_cache). Off: no disk reads, writes or cleanup; the small in-memory
  // cache keeps working. Existing cache files are left alone. Thread-safe, takes effect from the next lookup.
  void SetCacheEnabled(bool on);

  // Per-track timing nudge for the track that is currently published (ignored when `track_id` is not that
  // track). Kept in the cache entry, saved on the worker ~0.5 s after the last call, so it is safe to call on
  // every key press. With the disk cache off it is only remembered for this session.
  void SetTrackOffset(const std::wstring& track_id, int offset_ms);

  std::shared_ptr<const LyricsSnapshot> Snapshot() const;

  // %APPDATA%\IslandCapsule\lyrics
  static std::wstring CacheDir();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace island
