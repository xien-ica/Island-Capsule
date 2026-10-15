// Phase 1 – GSMTC media core implementation.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Graphics.Imaging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <thread>

#include "media/gsmtc.hpp"
#include "theme/accent.hpp"

namespace island {

namespace {

namespace wmc = winrt::Windows::Media::Control;
namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wss = winrt::Windows::Storage::Streams;

using Manager = wmc::GlobalSystemMediaTransportControlsSessionManager;
using Session = wmc::GlobalSystemMediaTransportControlsSession;
using PlaybackStatus = wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

// Max decoded thumbnail edge. The cover is decoded ONCE at this size (Fant, in the WinRT decoder); the accent is
// taken from it here on the worker, and the UI only has to box-filter it down to ~26 px x DPI scale.
// 128 px covers up to ~490% display scaling without upscaling.
constexpr int kArtPx = 128;

enum class Cmd { PlayPause, Next, Previous };

// Blocking WinRT calls get a deadline: a stuck player must not freeze the worker (and with it play / pause,
// track info and the cover) forever.
constexpr std::chrono::milliseconds kPropsTimeout{ 4000 };     // media properties
constexpr std::chrono::milliseconds kThumbTimeout{ 4000 };     // each cover-decode step
constexpr std::chrono::milliseconds kCommandTimeout{ 2000 };   // play / pause / next / previous
constexpr std::chrono::milliseconds kManagerTimeout{ 15000 };  // session manager at startup

struct AsyncTimeout {};

// Like op.get(), but gives up (cancelling the operation) after `timeout` by throwing AsyncTimeout. A failed
// operation still throws its own error from GetResults(), exactly as get() would.
template <typename Op>
auto GetWithTimeout(Op const& op, std::chrono::milliseconds timeout) {
  if (op.wait_for(timeout) == winrt::Windows::Foundation::AsyncStatus::Started) {
    try { op.Cancel(); } catch (...) {}
    throw AsyncTimeout{};
  }
  return op.GetResults();
}

std::wstring Lower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return s;
}

// Shared between the worker and the WinRT event handlers. Handlers hold a
// shared_ptr to this (never a raw Impl*), so a late-arriving event can never
// touch freed memory.
struct WakeState {
  std::mutex m;
  std::condition_variable cv;
  bool dirty = false;
  bool rebind = false;
  bool stop = false;
  bool tl_dirty = false;   // timeline changed (only subscribed to while lyrics mode is on)
  bool tl_cfg = false;     // SetTimelineEnabled() was called
  std::vector<Cmd> cmds;

  void Wake(bool need_rebind) {
    {
      std::lock_guard<std::mutex> lk(m);
      dirty = true;
      rebind = rebind || need_rebind;
    }
    cv.notify_one();
  }
  void WakeTimeline(bool cfg_change) {
    {
      std::lock_guard<std::mutex> lk(m);
      if (cfg_change) tl_cfg = true; else tl_dirty = true;
    }
    cv.notify_one();
  }
  void Post(Cmd c) {
    {
      std::lock_guard<std::mutex> lk(m);
      cmds.push_back(c);
    }
    cv.notify_one();
  }
  void RequestStop() {
    {
      std::lock_guard<std::mutex> lk(m);
      stop = true;
    }
    cv.notify_one();
  }
};

// Decode a thumbnail to a square kArtPx x kArtPx premultiplied BGRA image
// (aspect-preserving scale, then center crop, so 16:9 thumbnails don't distort), and extract its accent.
// Runs on the worker thread, once per track.
std::shared_ptr<const ArtImage> DecodeThumbnail(wss::IRandomAccessStreamReference const& ref) {
  if (!ref) return nullptr;
  try {
    auto stream = GetWithTimeout(ref.OpenReadAsync(), kThumbTimeout);
    auto decoder = GetWithTimeout(wgi::BitmapDecoder::CreateAsync(stream), kThumbTimeout);

    const uint32_t w0 = decoder.PixelWidth();
    const uint32_t h0 = decoder.PixelHeight();
    if (w0 == 0 || h0 == 0) return nullptr;

    // Never upscale a small cover: the square edge is min(kArtPx, shorter source side).
    const uint32_t edge = (std::min)(static_cast<uint32_t>(kArtPx), (std::min)(w0, h0));
    const double scale = static_cast<double>(edge) / static_cast<double>((std::min)(w0, h0));
    const uint32_t sw = (std::max)(edge, static_cast<uint32_t>(w0 * scale + 0.5));
    const uint32_t sh = (std::max)(edge, static_cast<uint32_t>(h0 * scale + 0.5));

    wgi::BitmapTransform tf;
    tf.ScaledWidth(sw);
    tf.ScaledHeight(sh);
    tf.InterpolationMode(wgi::BitmapInterpolationMode::Fant);

    auto provider = GetWithTimeout(decoder.GetPixelDataAsync(
        wgi::BitmapPixelFormat::Bgra8,
        wgi::BitmapAlphaMode::Premultiplied,
        tf,
        wgi::ExifOrientationMode::IgnoreExifOrientation,
        wgi::ColorManagementMode::DoNotColorManage), kThumbTimeout);

    auto pixels = provider.DetachPixelData();
    if (pixels.size() != static_cast<size_t>(sw) * sh * 4) return nullptr;

    auto img = std::make_shared<ArtImage>();
    const int n = static_cast<int>(edge);
    img->w = n;
    img->h = n;
    img->bgra.resize(static_cast<size_t>(n) * n * 4);

    const uint32_t x0 = (sw - edge) / 2;
    const uint32_t y0 = (sh - edge) / 2;
    for (int row = 0; row < n; ++row) {
      const uint8_t* src = pixels.data() + (static_cast<size_t>(y0) + row) * sw * 4 + static_cast<size_t>(x0) * 4;
      uint8_t* dst = img->bgra.data() + static_cast<size_t>(row) * n * 4;
      std::memcpy(dst, src, static_cast<size_t>(n) * 4);
    }

    const Accent acc = ExtractAccent(*img);   // ~2.3k samples, once per track, off the UI thread
    img->accent_r = acc.color.r;
    img->accent_g = acc.color.g;
    img->accent_b = acc.color.b;
    img->accent_strength = acc.strength;
    img->accent_ready = true;
    return img;
  } catch (...) {
    return nullptr;
  }
}

// Reads the playback timeline into `info` (position at "now", duration, rate). Used only to pick the
// current lyric line; nothing about it is ever drawn. Never throws.
void ReadTimeline(Session const& s, MediaInfo& info) {
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;
  info.has_timeline = false;
  info.duration_ms = 0;
  info.position_ms = 0;
  info.playback_rate = 1.0;
  try {
    auto tl = s.GetTimelineProperties();
    const int64_t start = duration_cast<milliseconds>(tl.StartTime()).count();
    const int64_t end = duration_cast<milliseconds>(tl.EndTime()).count();
    int64_t pos = duration_cast<milliseconds>(tl.Position()).count();
    if (start > 0 && pos >= start) pos -= start;

    double rate = 1.0;
    try {
      auto r = s.GetPlaybackInfo().PlaybackRate();
      if (r) rate = r.Value();
    } catch (...) {
    }
    if (!(rate > 0.05 && rate < 8.0)) rate = 1.0;

    // Position is "as of LastUpdatedTime": bring it up to now while the media is playing.
    if (info.is_playing) {
      int64_t age = 0;
      try {
        age = duration_cast<milliseconds>(winrt::clock::now() - tl.LastUpdatedTime()).count();
      } catch (...) {
      }
      if (age < 0 || age > 24LL * 3600 * 1000) age = 0;   // unset / bogus stamp: trust Position as is
      pos += static_cast<int64_t>(static_cast<double>(age) * rate);
    }

    info.duration_ms = end > start ? end - start : 0;
    info.position_ms = pos < 0 ? 0 : pos;
    info.position_stamp = std::chrono::steady_clock::now();
    info.playback_rate = rate;
    info.has_timeline = end > start || pos > 0;
  } catch (...) {
  }
}

} // namespace

// ---------------------------------------------------------------------------

struct MediaSession::Impl {
  std::shared_ptr<WakeState> wake = std::make_shared<WakeState>();
  std::thread worker;
  ChangedFn on_changed;

  // Guarded by `mtx`
  mutable std::mutex mtx;
  Session session{ nullptr };
  std::shared_ptr<const MediaInfo> snapshot = std::make_shared<const MediaInfo>();
  std::vector<std::wstring> preferred;   // lower-case substrings
  bool only_selected = false;
  std::vector<std::wstring> selected;    // lower-case substrings (".exe" stripped)

  // Worker-thread only (no lock needed)
  Manager manager{ nullptr };
  winrt::event_token mgr_current_tok{};
  winrt::event_token mgr_sessions_tok{};
  winrt::event_token props_tok{};
  winrt::event_token playback_tok{};
  winrt::event_token timeline_tok{};
  std::atomic<bool> tl_enabled{ false };   // lyrics mode on: read the timeline + listen for its changes
  std::wstring art_key;
  std::shared_ptr<const ArtImage> art_cache;

  Session CurrentSession() const {
    std::lock_guard<std::mutex> lk(mtx);
    return session;
  }

  Session PickSession() {
    if (!manager) return nullptr;
    std::vector<std::wstring> prefs, only;
    bool filter = false;
    {
      std::lock_guard<std::mutex> lk(mtx);
      prefs = preferred;
      filter = only_selected;
      only = selected;
    }

    if (filter) {
      // Only the selected apps count. Prefer Windows' current session if it is one of them, then
      // a playing one, then whatever matches first. No match = no media.
      auto matches = [&](Session const& s) {
        const std::wstring id = Lower(std::wstring(s.SourceAppUserModelId()));
        for (auto const& p : only)
          if (!p.empty() && id.find(p) != std::wstring::npos) return true;
        return false;
      };
      Session first{ nullptr }, playing{ nullptr };
      for (auto const& s : manager.GetSessions()) {
        if (!matches(s)) continue;
        if (!first) first = s;
        try {
          if (!playing && s.GetPlaybackInfo().PlaybackStatus() == PlaybackStatus::Playing) playing = s;
        } catch (...) {
        }
      }
      try {
        Session cur = manager.GetCurrentSession();
        if (cur && matches(cur)) return cur;
      } catch (...) {
      }
      return playing ? playing : first;
    }

    if (!prefs.empty()) {
      for (auto const& s : manager.GetSessions()) {
        const std::wstring id = Lower(std::wstring(s.SourceAppUserModelId()));
        for (auto const& p : prefs) {
          if (!p.empty() && id.find(p) != std::wstring::npos) return s;
        }
      }
    }
    return manager.GetCurrentSession();
  }

  void Unsubscribe(Session const& s) {
    if (!s) return;
    try {
      s.MediaPropertiesChanged(props_tok);
      s.PlaybackInfoChanged(playback_tok);
      if (timeline_tok) s.TimelinePropertiesChanged(timeline_tok);
    } catch (...) {
    }
    props_tok = {};
    playback_tok = {};
    timeline_tok = {};
  }

  // Subscribes to / unsubscribes from the current session's timeline events to match `tl_enabled`.
  // With lyrics mode off there is no timeline subscription at all (no extra wakeups).
  void ApplyTimelineSub() {
    Session s = CurrentSession();
    if (!s) return;
    try {
      if (tl_enabled.load() && !timeline_tok) {
        timeline_tok = s.TimelinePropertiesChanged([w = wake](auto&&, auto&&) { w->WakeTimeline(false); });
      } else if (!tl_enabled.load() && timeline_tok) {
        s.TimelinePropertiesChanged(timeline_tok);
        timeline_tok = {};
      }
    } catch (...) {
      timeline_tok = {};
    }
  }

  void Rebind() {
    Unsubscribe(CurrentSession());

    Session next{ nullptr };
    try {
      next = PickSession();
    } catch (...) {
    }

    if (next) {
      try {
        props_tok = next.MediaPropertiesChanged([w = wake](auto&&, auto&&) { w->Wake(false); });
        playback_tok = next.PlaybackInfoChanged([w = wake](auto&&, auto&&) { w->Wake(false); });
      } catch (...) {
        props_tok = {};
        playback_tok = {};
          }
    }

    {
      std::lock_guard<std::mutex> lk(mtx);
      session = next;
    }
    art_key.clear();
    art_cache = nullptr;
    ApplyTimelineSub();
  }

  void Refresh() {
    auto info = std::make_shared<MediaInfo>();
    Session s = CurrentSession();

    if (s) {
      try {
        info->has_session = true;
        info->source_app = std::wstring(s.SourceAppUserModelId());

        auto props = GetWithTimeout(s.TryGetMediaPropertiesAsync(), kPropsTimeout);
        info->title = std::wstring(props.Title());
        info->artist = std::wstring(props.Artist());
        info->album = std::wstring(props.AlbumTitle());

        auto pb = s.GetPlaybackInfo();
        info->is_playing = pb.PlaybackStatus() == PlaybackStatus::Playing;
        auto ctl = pb.Controls();
        info->can_play_pause = ctl.IsPlayPauseToggleEnabled() || ctl.IsPlayEnabled() || ctl.IsPauseEnabled();
        info->can_next = ctl.IsNextEnabled();
        info->can_prev = ctl.IsPreviousEnabled();
        if (tl_enabled.load()) ReadTimeline(s, *info);

        // Only decode the thumbnail when the track changed, or when the previous
        // attempt produced nothing (some players deliver the art a bit late).
        const std::wstring key = info->title + L"\x1f" + info->artist + L"\x1f" + info->album;
        if (key != art_key || !art_cache) {
          art_key = key;
          art_cache = DecodeThumbnail(props.Thumbnail());
        }
        info->art = art_cache;
      } catch (AsyncTimeout const&) {
        return;   // the player is not answering: keep the last snapshot (no blank flash); its next event refreshes again
      } catch (...) {
        // Player vanished mid-read; publish what we have.
      }
    }

    {
      std::lock_guard<std::mutex> lk(mtx);
      snapshot = info;
    }
    if (on_changed) on_changed();
  }

  // Timeline-only update: re-reads position / duration / rate into a copy of the last snapshot, without
  // touching properties or the thumbnail (cheap; this is what a seek or a progress tick from the player costs).
  void RefreshTimeline() {
    Session s = CurrentSession();
    if (!s) return;
    std::shared_ptr<const MediaInfo> cur;
    {
      std::lock_guard<std::mutex> lk(mtx);
      cur = snapshot;
    }
    if (!cur || !cur->has_session) return;
    auto info = std::make_shared<MediaInfo>(*cur);
    if (tl_enabled.load()) ReadTimeline(s, *info);
    {
      std::lock_guard<std::mutex> lk(mtx);
      snapshot = info;
    }
    if (on_changed) on_changed();
  }

  void RunCommands(std::vector<Cmd> const& cmds) {
    for (Cmd c : cmds) {
      Session s = CurrentSession();
      if (!s) continue;
      try {
        switch (c) {
          case Cmd::PlayPause: (void)GetWithTimeout(s.TryTogglePlayPauseAsync(), kCommandTimeout); break;
          case Cmd::Next:      (void)GetWithTimeout(s.TrySkipNextAsync(), kCommandTimeout); break;
          case Cmd::Previous:  (void)GetWithTimeout(s.TrySkipPreviousAsync(), kCommandTimeout); break;
        }
      } catch (...) {
      }
    }
  }

  void Run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    try {
      manager = GetWithTimeout(Manager::RequestAsync(), kManagerTimeout);
      mgr_current_tok = manager.CurrentSessionChanged([w = wake](auto&&, auto&&) { w->Wake(true); });
      mgr_sessions_tok = manager.SessionsChanged([w = wake](auto&&, auto&&) { w->Wake(true); });
    } catch (...) {
      manager = nullptr;
    }

    wake->Wake(true);   // initial bind + first snapshot

    for (;;) {
      bool do_rebind = false;
      bool full = false;      // full refresh: properties + thumbnail + playback state (+ timeline)
      bool tl_only = false;   // only the timeline moved
      bool tl_cfg = false;
      std::vector<Cmd> cmds;
      {
        std::unique_lock<std::mutex> lk(wake->m);
        wake->cv.wait(lk, [&] {
          return wake->stop || wake->dirty || wake->tl_dirty || wake->tl_cfg || !wake->cmds.empty();
        });
        if (wake->stop) break;
        do_rebind = wake->rebind;
        tl_cfg = wake->tl_cfg;
        full = wake->dirty || !wake->cmds.empty() || wake->tl_cfg;
        tl_only = wake->tl_dirty && !full;
        wake->rebind = false;
        wake->dirty = false;
        wake->tl_dirty = false;
        wake->tl_cfg = false;
        cmds.swap(wake->cmds);
      }

      try {
        if (do_rebind) Rebind();
        if (tl_cfg) ApplyTimelineSub();
        if (!cmds.empty()) RunCommands(cmds);
        if (full) Refresh();
        else if (tl_only) RefreshTimeline();
      } catch (...) {
      }
    }

    // Tear down on the thread that created the WinRT objects.
    try {
      if (manager) {
        manager.CurrentSessionChanged(mgr_current_tok);
        manager.SessionsChanged(mgr_sessions_tok);
      }
    } catch (...) {
    }
    Unsubscribe(CurrentSession());
    {
      std::lock_guard<std::mutex> lk(mtx);
      session = nullptr;
    }
    manager = nullptr;
    art_cache = nullptr;
    winrt::uninit_apartment();
  }
};

// ---------------------------------------------------------------------------

MediaSession::MediaSession() : impl_(std::make_unique<Impl>()) {}

MediaSession::~MediaSession() { Stop(); }

namespace {
// "Spotify.exe" -> "spotify": app ids look like "Spotify.exe", "Chrome", "MSEdge" or a Store
// package name, so the extension is optional for the user and dropped for matching.
std::wstring NormalizeApp(std::wstring s) {
  s = Lower(std::move(s));
  if (s.size() > 4 && s.compare(s.size() - 4, 4, L".exe") == 0) s.resize(s.size() - 4);
  return s;
}
} // namespace

void MediaSession::SetPreferredApps(std::vector<std::wstring> substrings) {
  for (auto& s : substrings) s = NormalizeApp(std::move(s));
  {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (impl_->preferred == substrings) return;
    impl_->preferred = std::move(substrings);
  }
  impl_->wake->Wake(true);
}

void MediaSession::SetTimelineEnabled(bool enabled) {
  if (impl_->tl_enabled.exchange(enabled) == enabled) return;
  impl_->wake->WakeTimeline(true);   // (un)subscribe + a refresh so position / duration show up right away
}

void MediaSession::SetSourceFilter(bool selected_only, std::vector<std::wstring> apps) {
  for (auto& s : apps) s = NormalizeApp(std::move(s));
  {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (impl_->only_selected == selected_only && impl_->selected == apps) return;
    impl_->only_selected = selected_only;
    impl_->selected = std::move(apps);
  }
  impl_->wake->Wake(true);   // re-pick the session now
}

bool MediaSession::Start(ChangedFn on_changed) {
  if (impl_->worker.joinable()) return true;
  impl_->on_changed = std::move(on_changed);
  {
    std::lock_guard<std::mutex> lk(impl_->wake->m);
    impl_->wake->stop = false;
  }
  try {
    impl_->worker = std::thread([impl = impl_.get()] { impl->Run(); });
  } catch (...) {
    return false;
  }
  return true;
}

void MediaSession::Stop() {
  if (!impl_ || !impl_->worker.joinable()) return;
  impl_->wake->RequestStop();
  impl_->worker.join();
}

std::shared_ptr<const MediaInfo> MediaSession::Snapshot() const {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  return impl_->snapshot;
}

void MediaSession::PlayPause() { impl_->wake->Post(Cmd::PlayPause); }
void MediaSession::Next()      { impl_->wake->Post(Cmd::Next); }
void MediaSession::Previous()  { impl_->wake->Post(Cmd::Previous); }

} // namespace island