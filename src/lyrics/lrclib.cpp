// Lyrics – LRCLIB client: disk cache + WinHTTP + search ranking on a background thread.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "lyrics/lrclib.hpp"
#include "lyrics/lyrics_cache.hpp"
#include "lyrics/lyrics_text.hpp"

#pragma comment(lib, "winhttp.lib")

namespace island {

namespace {

// LRCLIB asks clients to identify themselves. Put your repository URL here.
constexpr wchar_t kUserAgent[] = L"island-capsule/0.1 (https://github.com/xien-ica/Island-Capsule)";
constexpr wchar_t kHost[] = L"lrclib.net";
constexpr size_t kMaxBody = 4u * 1024u * 1024u;
constexpr int kMaxRequestsPerTrack = 5;
constexpr unsigned long long kNoneTtl100ns = 14ull * 24ull * 3600ull * 10000000ull;   // "not found" is re-tried after 14 days
constexpr size_t kCacheMaxFiles = 2000;                                   // disk cache cap: files...
constexpr unsigned long long kCacheMaxBytes = 16ull * 1024ull * 1024ull;   // ...and bytes (oldest are evicted first)
constexpr int kSweepEveryWrites = 25;                                      // eviction sweep: once at start, then every N writes
constexpr unsigned long long kTouchAfter100ns = 24ull * 3600ull * 10000000ull;   // a cache hit bumps the file time at most once a day
constexpr std::chrono::milliseconds kOffsetDebounce{ 500 };               // per-track nudges are saved this long after the last key press

// ---- cache --------------------------------------------------------------------------------------

std::wstring AppDataDir() {
  wchar_t base[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return L"";
  return std::wstring(base) + L"\\IslandCapsule";
}

bool ReadFileAll(const std::wstring& path, std::string* out, unsigned long long* age_100ns) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart <= 2 * 1024 * 1024;
  if (ok) {
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    ok = size.QuadPart == 0 || (ReadFile(f, out->data(), static_cast<DWORD>(size.QuadPart), &got, nullptr) &&
                                got == static_cast<DWORD>(size.QuadPart));
  }
  if (ok && age_100ns) {
    FILETIME wt{}, now{};
    GetFileTime(f, nullptr, nullptr, &wt);
    GetSystemTimeAsFileTime(&now);
    const unsigned long long a = (static_cast<unsigned long long>(wt.dwHighDateTime) << 32) | wt.dwLowDateTime;
    const unsigned long long b = (static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    *age_100ns = b > a ? b - a : 0;
  }
  CloseHandle(f);
  return ok;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
  const std::wstring tmp = path + L".tmp";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  const bool ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) &&
                  put == static_cast<DWORD>(data.size());
  CloseHandle(f);
  if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

// File format: see lyrics_cache.hpp (v2: magic, key, OK|NONE, offset, length, LRC text; v1 still readable).
enum class CacheHit { Miss, Lyrics, None };

// A file that is missing, corrupt, truncated, for another key or an expired "not found" is just a Miss.
CacheHit CacheRead(const std::wstring& path, const std::string& key, CacheEntry* e, unsigned long long* age_out) {
  std::string raw;
  unsigned long long age = 0;
  if (!ReadFileAll(path, &raw, &age)) return CacheHit::Miss;
  if (age_out) *age_out = age;
  if (!ParseCacheEntry(raw, key, e)) return CacheHit::Miss;
  if (!e->found) return age > kNoneTtl100ns ? CacheHit::Miss : CacheHit::None;
  return CacheHit::Lyrics;
}

void CacheWrite(const std::wstring& path, const std::string& key, const CacheEntry& e) {
  const std::string data = SerializeCacheEntry(key, e);
  if (data.empty()) return;
  const std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
  const std::wstring parent = dir.substr(0, dir.find_last_of(L'\\'));
  CreateDirectoryW(parent.c_str(), nullptr);
  CreateDirectoryW(dir.c_str(), nullptr);
  WriteFileAtomic(path, data);
}

// Sets the file's last-write time to "now" (so the oldest-first eviction behaves like an LRU for songs that
// keep being played). Only used for found-lyrics entries: a "not found" entry's age is its expiry clock.
void TouchFile(const std::wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  SetFileTime(f, nullptr, nullptr, &now);
  CloseHandle(f);
}

bool EndsWith(const std::wstring& s, const wchar_t* suffix) {
  const size_t n = wcslen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Keeps the cache folder inside kCacheMaxFiles / kCacheMaxBytes (oldest files go first) and removes leftover
// ".tmp" files from interrupted writes. Only touches *.txt / *.tmp inside the lyrics folder. Worker thread.
void SweepCache(const std::wstring& dir) {
  std::vector<CacheFileInfo> files;
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::wstring name = fd.cFileName;
    const std::wstring full = dir + L"\\" + name;
    if (EndsWith(name, L".tmp")) { DeleteFileW(full.c_str()); continue; }
    if (!EndsWith(name, L".txt")) continue;
    CacheFileInfo fi;
    fi.path = full;
    fi.size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    fi.written = (static_cast<unsigned long long>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
    files.push_back(std::move(fi));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  for (size_t i : PickEvictions(files, kCacheMaxFiles, kCacheMaxBytes)) DeleteFileW(files[i].path.c_str());
}

// ---- HTTP -----------------------------------------------------------------------------------------

struct HttpResult {
  int status = 0;          // 0 = could not reach the server / timeout / TLS failure
  std::string body;
};

struct Http {
  HINTERNET session = nullptr;
  HINTERNET connect = nullptr;

  bool Open() {
    // Automatic proxy (Windows 8.1+); fall back to the default WinHTTP proxy settings.
    session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
      session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 8000);   // resolve, connect, send, receive (ms)
#ifdef WINHTTP_OPTION_DECOMPRESSION
    {   // gzip / deflate: /api/search answers are big JSON documents that compress very well
      DWORD flags = WINHTTP_DECOMPRESSION_FLAG_ALL;
      WinHttpSetOption(session, WINHTTP_OPTION_DECOMPRESSION, &flags, sizeof(flags));
    }
#endif
#ifdef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
    {   // HTTP/2 where the OS supports it (ignored otherwise)
      DWORD flags = WINHTTP_PROTOCOL_FLAG_HTTP2;
      WinHttpSetOption(session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &flags, sizeof(flags));
    }
#endif
    connect = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    return connect != nullptr;
  }

  void Close() {
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    connect = session = nullptr;
  }

  HttpResult Get(const std::string& path_and_query) {
    HttpResult r;
    if (!connect) return r;
    const std::wstring wpath(path_and_query.begin(), path_and_query.end());   // already URL-encoded ASCII
    HINTERNET req = WinHttpOpenRequest(connect, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) return r;

    bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (ok) {
      DWORD code = 0, len = sizeof(code);
      if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX)) {
        r.status = static_cast<int>(code);
        for (;;) {
          DWORD avail = 0;
          if (!WinHttpQueryDataAvailable(req, &avail)) { r.status = 0; break; }
          if (avail == 0) break;
          if (r.body.size() + avail > kMaxBody) { r.status = 0; break; }
          const size_t old = r.body.size();
          r.body.resize(old + avail);
          DWORD got = 0;
          if (!WinHttpReadData(req, &r.body[old], avail, &got)) { r.status = 0; break; }
          r.body.resize(old + got);
          if (got == 0) break;
        }
      }
    }
    WinHttpCloseHandle(req);
    if (r.status == 0) r.body.clear();
    return r;
  }
};

std::string QueryGet(const std::string& artist, const std::string& track, const std::string& album, int dur) {
  std::string q = "/api/get?artist_name=" + UrlEncode(artist) + "&track_name=" + UrlEncode(track);
  if (!album.empty()) q += "&album_name=" + UrlEncode(album);
  if (dur > 0) q += "&duration=" + std::to_string(dur);
  return q;
}

std::string QuerySearch(const std::string& q) { return "/api/search?q=" + UrlEncode(q); }

} // namespace

// ---------------------------------------------------------------------------------------------------

std::wstring LyricsFetcher::CacheDir() {
  const std::wstring base = AppDataDir();
  return base.empty() ? L"lyrics" : base + L"\\lyrics";
}

struct LyricsFetcher::Impl {
  ChangedFn on_changed;
  std::thread worker;

  mutable std::mutex m;
  std::condition_variable cv;
  bool stop = false;
  bool has_req = false;
  LyricsRequest req;                       // pending
  std::atomic<unsigned long long> gen{ 0 };   // bumped by every Request()/Cancel(): running lookups compare it
  std::shared_ptr<const LyricsSnapshot> snap = std::make_shared<const LyricsSnapshot>();

  // Disk cache on/off (settings: lyrics_cache). Off = no disk reads, writes or sweeps; the in-memory cache stays.
  std::atomic<bool> cache_enabled{ true };

  // Per-track timing nudges waiting to be saved (guarded by m). Saved by the worker kOffsetDebounce after the
  // last SetTrackOffset(), or at shutdown, so key auto-repeat never turns into one file write per press.
  struct OffsetJob {
    std::string key;
    std::wstring path;
    int offset_ms = 0;
  };
  std::vector<OffsetJob> offs;
  std::chrono::steady_clock::time_point off_due{};
  std::string cur_key;       // cache entry of the PUBLISHED track (guarded by m)
  std::wstring cur_path;

  // Worker-thread only (no locking needed): ONE WinHTTP session / TLS connection reused for every track.
  // Opening a session (proxy auto-detection) + TLS handshake used to be paid again for every song.
  Http http;
  bool swept_once = false;
  int writes_since_sweep = 0;

  struct Outcome {
    LyricsState state = LyricsState::NotFound;
    bool network_error = false;
    std::shared_ptr<const std::vector<LyricLine>> lines;
    int offset_ms = 0;               // per-track nudge stored with the entry
    std::string cache_key;           // the cache entry this result lives in (target of offset saves)
    std::wstring cache_path;
    // Disk-cache work, done AFTER the result was published so it never delays the lyrics.
    bool write = false;
    bool write_found = false;
    std::wstring write_path;
    std::string write_key;
    std::string write_lrc;
    std::wstring touch_path;         // cache hit on an old entry: bump its file time
  };

  // Small in-memory LRU of finished lookups (parsed lines are shared, so this is cheap): going back to a
  // song, repeat / shuffle, or toggling lyrics mode never touches the disk or the network again.
  static constexpr size_t kMemCap = 12;
  std::vector<std::pair<std::string, Outcome>> mem;   // most recently used last

  const Outcome* MemFind(const std::string& key) {
    for (size_t i = 0; i < mem.size(); ++i) {
      if (mem[i].first != key) continue;
      if (i + 1 != mem.size()) std::rotate(mem.begin() + static_cast<std::ptrdiff_t>(i),
                                           mem.begin() + static_cast<std::ptrdiff_t>(i) + 1, mem.end());
      return &mem.back().second;
    }
    return nullptr;
  }

  void MemPut(const std::string& key, const Outcome& o) {
    Outcome light;   // without the write-back payload
    light.state = o.state;
    light.lines = o.lines;
    light.offset_ms = o.offset_ms;
    light.cache_key = o.cache_key;
    light.cache_path = o.cache_path;
    for (size_t i = 0; i < mem.size(); ++i)
      if (mem[i].first == key) { mem.erase(mem.begin() + static_cast<std::ptrdiff_t>(i)); break; }
    if (mem.size() >= kMemCap) mem.erase(mem.begin());
    mem.emplace_back(key, std::move(light));
  }

  void MemSetOffset(const std::string& cache_key, int offset_ms) {
    for (auto& e : mem)
      if (e.second.cache_key == cache_key) e.second.offset_ms = offset_ms;
  }

  void Publish(unsigned long long my_gen, const LyricsRequest& r, const Outcome& o) {
    {
      std::lock_guard<std::mutex> lk(m);
      if (gen.load() != my_gen) return;   // superseded while we were working: drop it
      auto s = std::make_shared<LyricsSnapshot>();
      s->track_id = r.track_id;
      s->duration_sec = r.duration_sec;
      s->state = o.state;
      s->network_error = o.network_error;
      s->lines = o.lines;
      s->track_offset_ms = o.offset_ms;
      snap = s;
      cur_key = o.state == LyricsState::Ready ? o.cache_key : std::string();
      cur_path = o.state == LyricsState::Ready ? o.cache_path : std::wstring();
    }
    if (on_changed) on_changed();
  }

  // Saves one per-track nudge: always into the memory cache, into the cache file when the disk cache is on.
  void ApplyOffsetJob(const OffsetJob& j) {
    MemSetOffset(j.key, j.offset_ms);
    if (!cache_enabled.load()) return;
    std::string raw;
    if (!ReadFileAll(j.path, &raw, nullptr)) return;
    CacheEntry e;
    if (!ParseCacheEntry(raw, j.key, &e) || !e.found || e.offset_ms == j.offset_ms) return;
    e.offset_ms = j.offset_ms;
    const std::string data = SerializeCacheEntry(j.key, e);
    if (!data.empty()) WriteFileAtomic(j.path, data);
  }

  void MaybeSweep() {
    if (!cache_enabled.load()) return;
    if (swept_once && writes_since_sweep < kSweepEveryWrites) return;
    SweepCache(LyricsFetcher::CacheDir());
    swept_once = true;
    writes_since_sweep = 0;
  }

  // The actual lookup. Never throws; checks `cancelled` between steps.
  Outcome Lookup(const LyricsRequest& r, unsigned long long my_gen) {
    Outcome out;
    auto cancelled = [&] { return stop || gen.load() != my_gen; };

    const std::wstring title = CleanTitle(r.title);
    const std::wstring artist = CleanArtist(r.artist);
    if (title.empty()) return out;

    // cache
    const int dur = (std::max)(0, r.duration_sec);
    const std::string key_base = WideToUtf8(NormalizeName(artist)) + "|" + WideToUtf8(NormalizeName(title)) + "|";
    const std::string key = key_base + std::to_string(dur);
    if (const Outcome* m = MemFind(key)) return *m;   // seen this session: instant

    const bool use_disk = cache_enabled.load();
    const std::wstring dir = LyricsFetcher::CacheDir();
    auto path_for = [&](const std::string& k) { return dir + L"\\" + Utf8ToWide(Hex64(Fnv1a64(k))) + L".txt"; };
    const std::wstring path = path_for(key);

    if (use_disk) {
      // Exact duration first, then +-1 s: players round the length differently, and that alone must not cost
      // a network request. Only found-lyrics entries count for the neighbours (a "not found" is only trusted
      // for the exact key).
      for (int probe = 0; probe < 3; ++probe) {
        const int d = probe == 0 ? dur : probe == 1 ? dur - 1 : dur + 1;
        if (probe > 0 && (dur <= 0 || d <= 0)) continue;
        const std::string ck = probe == 0 ? key : key_base + std::to_string(d);
        const std::wstring cp = probe == 0 ? path : path_for(ck);
        CacheEntry e;
        unsigned long long age = 0;
        const CacheHit hit = CacheRead(cp, ck, &e, &age);
        if (hit == CacheHit::None && probe == 0) { MemPut(key, out); return out; }
        if (hit != CacheHit::Lyrics) continue;
        auto lines = ParseLrc(Utf8ToWide(e.lrc));
        if (lines.empty()) continue;   // unusable text: treat as a miss, the network result overwrites it
        out.state = LyricsState::Ready;
        out.lines = std::make_shared<const std::vector<LyricLine>>(std::move(lines));
        out.offset_ms = e.offset_ms;
        out.cache_key = ck;
        out.cache_path = cp;
        if (age > kTouchAfter100ns) out.touch_path = cp;
        MemPut(key, out);
        return out;
      }
    }

    const std::string a8 = WideToUtf8(artist), t8 = WideToUtf8(title), alb8 = WideToUtf8(r.album);
    if (!http.connect && !http.Open()) {   // normally already open from the previous track
      http.Close();
      out.network_error = true;
      return out;
    }

    int requests = 0;
    bool net_err = false;
    bool definitive_none = false;   // LRCLIB says: instrumental
    std::string found_lrc;

    auto fetch = [&](const std::string& pq) -> HttpResult {
      if (cancelled() || requests >= kMaxRequestsPerTrack) return HttpResult{ -1, {} };   // -1 = skipped
      ++requests;
      HttpResult h = http.Get(pq);
      if (h.status == 0) net_err = true;
      return h;
    };

    // exact lookup: /api/get
    auto try_get = [&](const std::string& ar, const std::string& tr, const std::string& al, int d) {
      HttpResult h = fetch(QueryGet(ar, tr, al, d));
      if (h.status != 200) return false;
      Json j;
      LyricsCandidate c;
      if (!ParseJson(h.body, &j) || !CandidateFromJson(j, &c)) return false;
      if (c.instrumental) { definitive_none = true; return false; }
      if (c.synced.empty()) return false;
      found_lrc = c.synced;
      return true;
    };

    // fuzzy lookup: /api/search?q=
    auto try_search = [&](const std::string& q, const std::wstring& want_title, const std::wstring& want_artist) {
      HttpResult h = fetch(QuerySearch(q));
      if (h.status != 200) return false;
      Json j;
      if (!ParseJson(h.body, &j) || j.type != Json::Type::Array) return false;
      std::vector<LyricsCandidate> list;
      for (const Json& it : j.items) {
        LyricsCandidate c;
        if (CandidateFromJson(it, &c)) list.push_back(std::move(c));
      }
      const int best = PickBestCandidate(list, want_title, want_artist, r.duration_sec);
      if (best < 0) return false;
      found_lrc = list[static_cast<size_t>(best)].synced;
      return true;
    };

    bool found = false;
    if (!artist.empty()) {
      found = try_get(a8, t8, alb8, r.duration_sec);
      if (!found && !definitive_none && !alb8.empty() && !cancelled()) found = try_get(a8, t8, "", r.duration_sec);
    }
    if (!found && !definitive_none && !cancelled())
      found = try_search(artist.empty() ? t8 : a8 + " " + t8, title, artist);
    if (!found && !definitive_none && !cancelled() && !artist.empty()) {
      const std::wstring first = FirstArtist(artist);
      if (first != artist) found = try_search(WideToUtf8(first) + " " + t8, title, first);
    }
    if (!found && !definitive_none && !cancelled()) {
      // YouTube style "Artist - Title" in the title field
      const size_t dash = title.find(L" - ");
      if (dash != std::wstring::npos && dash > 0) {
        const std::wstring a2 = TrimSpace(title.substr(0, dash)), t2 = TrimSpace(title.substr(dash + 3));
        if (!a2.empty() && !t2.empty())
          found = try_search(WideToUtf8(a2) + " " + WideToUtf8(t2), t2, a2);
      }
    }
    if (net_err) http.Close();   // broken connection: the next lookup opens a fresh one

    if (cancelled()) return out;   // result is dropped by Publish anyway; do not cache a half-finished lookup

    if (found) {
      auto lines = ParseLrc(Utf8ToWide(found_lrc));
      if (!lines.empty()) {
        out.state = LyricsState::Ready;
        out.lines = std::make_shared<const std::vector<LyricLine>>(std::move(lines));
        out.cache_key = key;
        out.cache_path = path;
        out.write = use_disk;
        out.write_found = true;
        out.write_path = path;
        out.write_key = key;
        out.write_lrc = std::move(found_lrc);
        MemPut(key, out);
        return out;
      }
    }
    if (net_err && !definitive_none) {
      out.network_error = true;   // not cached: try again next time this song plays
      return out;
    }
    out.write = use_disk;
    out.write_found = false;
    out.write_path = path;
    out.write_key = key;
    MemPut(key, out);
    return out;
  }

  void Run() {
    for (;;) {
      LyricsRequest r;
      unsigned long long my_gen = 0;
      std::vector<OffsetJob> jobs;
      bool stopping = false, have_req = false;
      {
        std::unique_lock<std::mutex> lk(m);
        for (;;) {
          if (stop) { stopping = true; break; }
          if (has_req) { have_req = true; break; }
          if (!offs.empty()) {
            if (std::chrono::steady_clock::now() >= off_due) break;   // nudge save is due
            cv.wait_until(lk, off_due);
          } else {
            cv.wait(lk);
          }
        }
        if (!have_req) jobs.swap(offs);   // due, or flush what is pending on the way out
        if (have_req) {
          r = req;
          my_gen = gen.load();
          has_req = false;
        }
      }
      for (const OffsetJob& j : jobs) ApplyOffsetJob(j);
      if (stopping) { http.Close(); return; }
      if (!have_req) continue;

      Outcome o;
      try {
        o = Lookup(r, my_gen);
      } catch (...) {
        o = Outcome{};
        o.network_error = true;
      }
      Publish(my_gen, r, o);                       // the UI gets the lyrics first...
      if (cache_enabled.load()) {                  // ...then the disk cache
        if (o.write) {
          CacheWrite(o.write_path, o.write_key, CacheEntry{ o.write_found, 0, o.write_lrc });
          ++writes_since_sweep;
        }
        if (!o.touch_path.empty()) TouchFile(o.touch_path);
        MaybeSweep();
      }
    }
  }
};


LyricsFetcher::LyricsFetcher() : impl_(std::make_unique<Impl>()) {}
LyricsFetcher::~LyricsFetcher() { Stop(); }

bool LyricsFetcher::Start(ChangedFn on_changed) {
  if (impl_->worker.joinable()) return true;
  impl_->on_changed = std::move(on_changed);
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->stop = false;
  }
  try {
    impl_->worker = std::thread([p = impl_.get()] { p->Run(); });
  } catch (...) {
    return false;
  }
  return true;
}

void LyricsFetcher::Stop() {
  if (!impl_ || !impl_->worker.joinable()) return;
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->stop = true;
  }
  impl_->cv.notify_one();
  impl_->worker.join();
}

void LyricsFetcher::Request(const LyricsRequest& r) {
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    const auto& cur = *impl_->snap;
    if (cur.state != LyricsState::Idle && cur.track_id == r.track_id && cur.duration_sec == r.duration_sec) return;
    impl_->gen.fetch_add(1);
    impl_->req = r;
    impl_->has_req = true;
    auto s = std::make_shared<LyricsSnapshot>();
    s->track_id = r.track_id;
    s->duration_sec = r.duration_sec;
    s->state = LyricsState::Loading;
    impl_->snap = s;
  }
  impl_->cv.notify_one();
  if (impl_->on_changed) impl_->on_changed();
}

void LyricsFetcher::Cancel() {
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    if (impl_->snap->state == LyricsState::Idle && !impl_->has_req) return;
    impl_->gen.fetch_add(1);
    impl_->has_req = false;
    impl_->snap = std::make_shared<const LyricsSnapshot>();
  }
  if (impl_->on_changed) impl_->on_changed();
}

void LyricsFetcher::SetCacheEnabled(bool on) { impl_->cache_enabled.store(on); }

void LyricsFetcher::SetTrackOffset(const std::wstring& track_id, int offset_ms) {
  offset_ms = (std::clamp)(offset_ms, -kMaxTrackOffsetMs, kMaxTrackOffsetMs);
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    const auto& cur = *impl_->snap;
    if (cur.state != LyricsState::Ready || cur.track_id != track_id || impl_->cur_key.empty()) return;   // stale
    bool merged = false;
    for (auto& j : impl_->offs)
      if (j.key == impl_->cur_key) { j.offset_ms = offset_ms; merged = true; break; }
    if (!merged) impl_->offs.push_back(Impl::OffsetJob{ impl_->cur_key, impl_->cur_path, offset_ms });
    impl_->off_due = std::chrono::steady_clock::now() + kOffsetDebounce;
  }
  impl_->cv.notify_one();
}

std::shared_ptr<const LyricsSnapshot> LyricsFetcher::Snapshot() const {
  std::lock_guard<std::mutex> lk(impl_->m);
  return impl_->snap;
}

} // namespace island