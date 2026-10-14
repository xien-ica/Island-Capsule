#pragma once
// Phase 1 – GSMTC media core (System Media Transport Controls).
//
// Threading model:
//   * All WinRT work (session discovery, property reads, thumbnail decoding,
//     transport commands) happens on ONE background worker thread.
//   * The UI thread only calls Snapshot() (cheap, lock + shared_ptr copy) and
//     PlayPause()/Next()/Previous() (just queue a command, never block).
//   * on_changed is invoked on the worker thread: post a message to your UI
//     thread from it, do not touch UI objects directly.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "media/media_info.hpp"

namespace island {

class MediaSession {
public:
  using ChangedFn = std::function<void()>;

  MediaSession();
  ~MediaSession();
  MediaSession(const MediaSession&) = delete;
  MediaSession& operator=(const MediaSession&) = delete;

  // Optional: prefer sessions whose app id contains one of these substrings
  // (case-insensitive), e.g. {L"spotify", L"chrome"}. Empty = Windows' choice.
  void SetPreferredApps(std::vector<std::wstring> substrings);

  // Source filter (like the visualizer's "Audio source" option): when `selected_only` is true only
  // sessions whose app id contains one of `apps` (case-insensitive, ".exe" optional, e.g.
  // {L"Spotify.exe", L"chrome.exe"}) are used; anything else counts as "no media".
  // Safe to call at any time (hot-reload): the worker re-picks its session.
  void SetSourceFilter(bool selected_only, std::vector<std::wstring> apps);

  // Lyrics mode only: when true the playback position / duration / rate are read into MediaInfo and
  // timeline changes wake the worker. When false (default) the timeline is never touched and there is no
  // timeline subscription. The position is used internally for lyric timing; it is never drawn.
  void SetTimelineEnabled(bool enabled);

  bool Start(ChangedFn on_changed);
  void Stop();

  std::shared_ptr<const MediaInfo> Snapshot() const;

  void PlayPause();
  void Next();
  void Previous();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace island
