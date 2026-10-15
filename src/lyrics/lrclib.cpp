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
#include <cstdlib>
#include <deque>
#include <random>
#include <set>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "lyrics/lrclib.hpp"
#include "lyrics/lyrics_cache.hpp"
#include "lyrics/lyrics_text.hpp"
#include "island_version.hpp"   // ISLAND_VERSION_W: generated from project(... VERSION x.y.z) in CMakeLists.txt

#pragma comment(lib, "winhttp.lib")

namespace island {

namespace {

// LRCLIB asks clients to identify themselves: app name, version, project link. The version follows the CMake project version.
constexpr wchar_t kUserAgent[] = L"island-capsule/" ISLAND_VERSION_W L" (https://github.com/xien-ica/Island-Capsule)";
constexpr wchar_t kHost[] = L"lrclib.net";
constexpr size_t kMaxBody = 4u * 1024u * 1024u;
constexpr int kMaxRequestsPerTrack = 4;   // worst case per lookup: exact get, search, search by first artist, "Artist - Title" search
// Politeness towards LRCLIB (a free, donation-run service). The client throttles ITSELF so it never depends on the server's limiter:
constexpr std::chrono::milliseconds kMinRequestGap{ 500 };   // minimum spacing between two requests
constexpr int kBudgetRequests = 20;                          // rolling budget: at most this many requests...
constexpr std::chrono::seconds kBudgetWindow{ 60 };          // ...per minute (a lookup just waits for a free slot)
// Back-off after the server refuses us. Retry-After is a minimum and is never shortened; consecutive refusals double the pause.
constexpr int kRate429BaseSec = 5;     // 429: at least this long, even when the server says "1 s" (it is already unhappy with us)
constexpr int kRate429CapSec = 120;    // ...doubling per consecutive refusal up to this
constexpr int kRate503CapSec = 30;     // 503 (overloaded): starts at Retry-After (default 1 s), doubles up to this
constexpr int kMaxRateRetries = 3;   // automatic re-tries of the CURRENT track after a 429 / pause (then the next play tries again)
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

bool CacheWrite(const std::wstring& path, const std::string& key, const CacheEntry& e) {
  const std::string data = SerializeCacheEntry(key, e);
  if (data.empty()) return false;
  const std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
  const std::wstring parent = dir.substr(0, dir.find_last_of(L'\\'));
  CreateDirectoryW(parent.c_str(), nullptr);
  CreateDirectoryW(dir.c_str(), nullptr);
  return WriteFileAtomic(path, data);
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
  unsigned long err = 0;   // GetLastError() of the failing WinHTTP call (diagnostics): 12002 timeout, 12007 DNS, 12029 connect, 12175 TLS ...
  unsigned ms = 0;         // how long the request took (diagnostics)
  int retry_after_s = 0;   // Retry-After header of a 429 / 503 answer (seconds; 0 = not given)
};

// Diagnostics: one line to the console (no-op without a console, e.g. the silent release build).
void DiagLog(const std::wstring& line) {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0, written = 0;
  if (out && out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode))
    WriteConsoleW(out, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
}

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
    const ULONGLONG t0 = GetTickCount64();
    const std::wstring wpath(path_and_query.begin(), path_and_query.end());   // already URL-encoded ASCII
    HINTERNET req = WinHttpOpenRequest(connect, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) { r.err = GetLastError(); return r; }

    bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (!ok) r.err = GetLastError();
    if (ok) {
      DWORD code = 0, len = sizeof(code);
      if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX)) {
        r.status = static_cast<int>(code);
        if (code == 429 || code == 503) {   // "slow down": remember how long the server wants us to wait
          wchar_t ra[32] = {};
          DWORD ra_len = sizeof(ra) - sizeof(wchar_t);
          if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"Retry-After", ra, &ra_len, WINHTTP_NO_HEADER_INDEX))
            r.retry_after_s = _wtoi(ra);
        }
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
    if (r.status == 0 && r.err == 0) r.err = GetLastError();   // failed while reading the body
    WinHttpCloseHandle(req);
    if (r.status == 0) r.body.clear();
    r.ms = static_cast<unsigned>(GetTickCount64() - t0);
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

// Human-readable kind of a request path, for the diagnostics log.
const wchar_t* QueryKind(const std::string& pq) { return pq.compare(0, 8, "/api/get") == 0 ? L"exact match" : L"search"; }

} // namespace

// ---------------------------------------------------------------------------------------------------

std::wstring LyricsFetcher::CacheDir() {
  const std::wstring base = AppDataDir();
  return base.empty() ? L"lyrics" : base + L"\\lyrics";
}

int LyricsFetcher::ClearDiskCache() {
  const std::wstring dir = CacheDir();
  if (dir.empty()) return 0;
  int removed = 0;
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::wstring name = fd.cFileName;
    if (!EndsWith(name, L".txt") && !EndsWith(name, L".tmp")) continue;
    if (DeleteFileW((dir + L"\\" + name).c_str())) ++removed;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return removed;
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
  std::chrono::steady_clock::time_point rate_until{};   // after a 429: no lookups before this (worker thread only)

  // Deferred re-try of the current track after a 429 (guarded by m, except retry_attempts: worker only).
  bool retry_pending = false;
  LyricsRequest retry_req;
  unsigned long long retry_gen = 0;
  std::chrono::steady_clock::time_point retry_due{};
  int retry_attempts = 0;

  // Worker thread only.
  int limited_streak = 0;                                        // consecutive 429 / 503 answers (reset by any 200 / 404)
  std::deque<std::chrono::steady_clock::time_point> req_times;   // send times inside the rolling request budget window
  std::set<std::string> done_queries;                            // queries that already got a definitive answer in THIS lookup (kept across its re-tries)

  // ms until the rate-limit pause is over (+ a small margin); 0 = not paused.
  int RateRetryMs() const {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(rate_until - std::chrono::steady_clock::now()).count();
    return left > 0 ? static_cast<int>(left) + 250 : 0;
  }

  // The server refused with 429 / 503: pause lookups. Retry-After is honored as a minimum (never shortened), consecutive
  // refusals double the pause (exponential back-off), and a little jitter keeps several clients from retrying in lockstep.
  void NoteLimited(int status, int retry_after_s) {
    ++limited_streak;
    retry_after_s = (std::min)((std::max)(retry_after_s, 0), 86400);
    const bool tooMany = status == 429;
    const int cap = tooMany ? kRate429CapSec : kRate503CapSec;
    long long wait_s = (std::max)(retry_after_s, tooMany ? kRate429BaseSec : 1);
    for (int i = 1; i < limited_streak && wait_s < cap; ++i) wait_s *= 2;
    wait_s = (std::min<long long>)(wait_s, (std::max)(cap, retry_after_s));
    static thread_local std::mt19937 rng{ std::random_device{}() };
    const int jitter_ms = std::uniform_int_distribution<int>(0, 400)(rng);
    rate_until = std::chrono::steady_clock::now() + std::chrono::seconds(wait_s) + std::chrono::milliseconds(jitter_ms);
    DiagLog(std::wstring(L"[lyrics] LRCLIB ") + (tooMany ? L"rate limit (429)" : L"overloaded (503)") + L": pausing lookups for " +
            std::to_wstring(wait_s) + L" s (" +
            (retry_after_s > 0 ? L"server Retry-After: " + std::to_wstring(retry_after_s) + L" s" : std::wstring(L"no Retry-After header")) +
            L", back-off step " + std::to_wstring(limited_streak) + L")\r\n");
  }
  std::chrono::steady_clock::time_point last_req{};     // when the last request finished: LRCLIB asks for 200-500 ms between requests
  bool swept_once = false;
  int writes_since_sweep = 0;

  struct Outcome {
    LyricsState state = LyricsState::NotFound;
    std::wstring label;              // "Artist - Title", for the diagnostics log
    bool network_error = false;
    int retry_in_ms = 0;             // > 0: the lookup was blocked / refused by a rate limit; try the same track again after this long
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
    const std::wstring label = artist.empty() ? title : artist + L" - " + title;
    out.label = label;

    // cache
    const int dur = (std::max)(0, r.duration_sec);
    const std::string key_base = WideToUtf8(NormalizeName(artist)) + "|" + WideToUtf8(NormalizeName(title)) + "|";
    const std::string key = key_base + std::to_string(dur);
    if (const Outcome* m = MemFind(key)) {   // seen this session: instant
      if (m->state == LyricsState::Ready) DiagLog(L"[lyrics] cache hit (memory): " + label + L"\r\n");
      return *m;
    }

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
        if (hit == CacheHit::None && probe == 0) {
          DiagLog(L"[lyrics] cache hit: " + label + L" is remembered as \"not found\" (looked up again after 14 days)\r\n");
          MemPut(key, out);
          return out;
        }
        if (hit != CacheHit::Lyrics) continue;
        auto lines = ParseLrc(Utf8ToWide(e.lrc));
        if (lines.empty()) continue;   // unusable text: treat as a miss, the network result overwrites it
        out.state = LyricsState::Ready;
        out.lines = std::make_shared<const std::vector<LyricLine>>(std::move(lines));
        out.offset_ms = e.offset_ms;
        out.cache_key = ck;
        out.cache_path = cp;
        if (age > kTouchAfter100ns) out.touch_path = cp;
        DiagLog(L"[lyrics] cache hit (disk): " + label + L" (" + std::to_wstring(out.lines->size()) + L" lines)\r\n");
        MemPut(key, out);
        return out;
      }
    }

    const std::string a8 = WideToUtf8(artist), t8 = WideToUtf8(title), alb8 = WideToUtf8(r.album);
    if (std::chrono::steady_clock::now() < rate_until) {   // LRCLIB told us to slow down: do not hammer it, and do not cache a miss
      out.network_error = true;
      out.retry_in_ms = RateRetryMs();
      return out;
    }
    if (!http.connect && !http.Open()) {   // normally already open from the previous track
      http.Close();
      out.network_error = true;
      return out;
    }

    DiagLog(std::wstring(L"[lyrics] not in local cache") + (use_disk ? L"" : L" (disk cache is off)") + L": asking LRCLIB for " + label + L"\r\n");
    int requests = 0;
    bool net_err = false;           // could not reach the server (timeout / DNS / TLS): the connection is dropped
    bool server_err = false;        // the server answered with a temporary failure (408 / 429 / 5xx)
    bool definitive_none = false;   // LRCLIB says: instrumental
    std::string found_lrc;
    std::wstring via;               // which kind of request produced the lyrics (diagnostics)
    bool budget_logged = false;

    auto fetch = [&](const std::string& pq) -> HttpResult {
      // A query that already got a definitive answer (200 / 404) earlier in this lookup is not asked again: after a 429 / 503
      // only the queries that did not complete are re-sent.
      if (done_queries.count(pq)) {
        DiagLog(std::wstring(L"[lyrics]   ") + QueryKind(pq) + L": answered earlier, not re-sent\r\n");
        return HttpResult{ 404, {} };
      }
      // After the first temporary failure the remaining steps would only wait for the same timeout / error again
      // (up to 4 more requests of 5-8 s each while offline), so the lookup stops there.
      if (cancelled() || requests >= kMaxRequestsPerTrack || net_err || server_err) return HttpResult{ -1, {} };   // -1 = skipped
      for (;;) {   // pacing: minimum gap between requests (also across songs) + rolling per-minute budget
        if (cancelled()) return HttpResult{ -1, {} };
        const auto now = std::chrono::steady_clock::now();
        while (!req_times.empty() && now - req_times.front() >= kBudgetWindow) req_times.pop_front();
        auto due = last_req + kMinRequestGap;
        const bool over_budget = req_times.size() >= static_cast<size_t>(kBudgetRequests);
        if (over_budget) due = (std::max)(due, req_times.front() + kBudgetWindow);
        if (now >= due) break;
        if (over_budget && !budget_logged) {
          budget_logged = true;
          DiagLog(L"[lyrics] request budget reached (" + std::to_wstring(kBudgetRequests) + L" per minute): waiting for a free slot\r\n");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      ++requests;
      req_times.push_back(std::chrono::steady_clock::now());
      HttpResult h = http.Get(pq);
      last_req = std::chrono::steady_clock::now();
      if (h.status == 200 || h.status == 404) {
        limited_streak = 0;
        done_queries.insert(pq);
        DiagLog(std::wstring(L"[lyrics]   ") + QueryKind(pq) + L" -> " + (h.status == 200 ? L"200 OK" : L"404 no match") + L" (" +
                std::to_wstring(h.ms) + L" ms)\r\n");
      } else {   // diagnostics: why a lookup ends as "network error"
        DiagLog(L"[lyrics] request failed: " +
                (h.status == 0 ? L"no response, WinHTTP error " + std::to_wstring(h.err)
                               : L"HTTP " + std::to_wstring(h.status)) +
                L" after " + std::to_wstring(h.ms) + L" ms  " + Utf8ToWide(pq) + L"\r\n");
      }
      if (h.status == 429 || h.status == 503) NoteLimited(h.status, h.retry_after_s);
      if (h.status == 0) net_err = true;
      else if (h.status == 408 || h.status == 429 || h.status >= 500) server_err = true;
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
      via = L"exact match";
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
      via = L"search";
      return true;
    };

    bool found = false;
    if (!artist.empty()) {
      // One exact lookup only. The former second exact lookup without the album is gone: the search below returns every
      // album's version and the best one is picked locally by title / artist / duration, so it cost a request for nothing.
      found = try_get(a8, t8, alb8, r.duration_sec);
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
        DiagLog(L"[lyrics] fetched synced lyrics: " + label + L" (" + std::to_wstring(lines.size()) + L" lines, " + via + L", " +
                std::to_wstring(requests) + L" request" + (requests == 1 ? L"" : L"s") + L")\r\n");
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
    if ((net_err || server_err) && !definitive_none) {
      // Not cached: try again next time this song plays. Only a clean "not found" answer (404 / no synced lyrics)
      // is remembered below; an LRCLIB outage or rate limit (429 / 5xx) must not blank songs for 14 days.
      out.network_error = true;
      out.retry_in_ms = RateRetryMs();   // only set while a 429 pause is active; plain outages are not re-tried here
      return out;
    }
    DiagLog(L"[lyrics] no synced lyrics on LRCLIB for " + label + (definitive_none ? L" (instrumental)" : L"") + L"\r\n");
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
      bool stopping = false, have_req = false, is_retry = false;
      {
        std::unique_lock<std::mutex> lk(m);
        for (;;) {
          if (stop) { stopping = true; break; }
          if (has_req) { have_req = true; break; }
          if (retry_pending && gen.load() != retry_gen) retry_pending = false;   // a newer track / Cancel() replaced it
          const auto now = std::chrono::steady_clock::now();
          if (retry_pending && now >= retry_due) {   // the rate-limit pause is over: look the current track up again
            r = retry_req;
            my_gen = retry_gen;
            retry_pending = false;
            have_req = is_retry = true;
            break;
          }
          if (!offs.empty() && now >= off_due) break;   // nudge save is due
          bool timed = false;
          std::chrono::steady_clock::time_point wake{};
          if (!offs.empty()) { timed = true; wake = off_due; }
          if (retry_pending && (!timed || retry_due < wake)) { timed = true; wake = retry_due; }
          if (timed) cv.wait_until(lk, wake); else cv.wait(lk);
        }
        if (!have_req) jobs.swap(offs);   // due, or flush what is pending on the way out
        if (have_req && !is_retry) {
          r = req;
          my_gen = gen.load();
          has_req = false;
          retry_attempts = 0;
          done_queries.clear();   // a new track: nothing answered yet
          retry_pending = false;
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
          const bool saved = CacheWrite(o.write_path, o.write_key, CacheEntry{ o.write_found, 0, o.write_lrc });
          ++writes_since_sweep;
          const std::wstring file = o.write_path.substr(o.write_path.find_last_of(L'\\') + 1);
          if (!saved)
            DiagLog(L"[lyrics] could not write cache file " + file + L"\r\n");
          else if (o.write_found)
            DiagLog(L"[lyrics] saved to cache: " + o.label + L" -> " + file + L" (" +
                    std::to_wstring((o.write_lrc.size() + 1023) / 1024) + L" KB)\r\n");
          else
            DiagLog(L"[lyrics] saved to cache: " + o.label + L" as \"not found\" (looked up again after 14 days) -> " + file + L"\r\n");
        }
        if (!o.touch_path.empty()) TouchFile(o.touch_path);
        MaybeSweep();
      }
      if (o.retry_in_ms > 0 && retry_attempts < kMaxRateRetries) {   // blocked by a 429 pause: try this track again later
        std::lock_guard<std::mutex> lk(m);
        if (gen.load() == my_gen && !has_req) {
          ++retry_attempts;
          retry_req = r;
          retry_gen = my_gen;
          retry_due = std::chrono::steady_clock::now() + std::chrono::milliseconds(o.retry_in_ms);
          retry_pending = true;
          DiagLog(L"[lyrics] will retry this track in " + std::to_wstring((o.retry_in_ms + 500) / 1000) + L" s (attempt " +
                  std::to_wstring(retry_attempts) + L"/" + std::to_wstring(kMaxRateRetries) + L")\r\n");
        }
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
    if (cur.state != LyricsState::Idle && cur.track_id == r.track_id) {
      if (cur.duration_sec == r.duration_sec) return;
      // Same song, only the reported length moved (players correct it, esp. around the end of the song): keep what we
      // have instead of starting a second lookup that can only cost another request.
      if (cur.state == LyricsState::Ready || std::abs(cur.duration_sec - r.duration_sec) <= 1) return;
    }
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