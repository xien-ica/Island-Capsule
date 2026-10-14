// Lyrics disk cache - pure parts: file format + eviction choice (see lyrics_cache.hpp).

#include "lyrics/lyrics_cache.hpp"

#include <algorithm>
#include <numeric>

namespace island {

namespace {

constexpr char kMagicV1[] = "island-lyrics-1";
constexpr char kMagicV2[] = "island-lyrics-2";

// Reads one '\n'-terminated line starting at *p (a trailing '\r' is dropped). False when there is no
// terminator, i.e. the file ends inside the header.
bool NextLine(const std::string& raw, size_t* p, std::string* out) {
  const size_t e = raw.find('\n', *p);
  if (e == std::string::npos) return false;
  out->assign(raw, *p, e - *p);
  if (!out->empty() && out->back() == '\r') out->pop_back();
  *p = e + 1;
  return true;
}

// Strict integer: optional '-', 1..12 digits, nothing else.
bool ParseInt(const std::string& s, long long lo, long long hi, long long* out) {
  size_t i = 0;
  bool neg = false;
  if (i < s.size() && s[i] == '-') { neg = true; ++i; }
  if (i >= s.size() || s.size() - i > 12) return false;
  long long v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  if (neg) v = -v;
  if (v < lo || v > hi) return false;
  *out = v;
  return true;
}

} // namespace

std::string SerializeCacheEntry(const std::string& key, const CacheEntry& e) {
  if (key.find_first_of("\r\n") != std::string::npos) return std::string();
  const int off = e.found ? (std::clamp)(e.offset_ms, -kMaxTrackOffsetMs, kMaxTrackOffsetMs) : 0;
  static const std::string kEmpty;
  const std::string& body = e.found ? e.lrc : kEmpty;
  std::string out;
  out.reserve(body.size() + key.size() + 64);
  out += kMagicV2;
  out += '\n';
  out += key;
  out += '\n';
  out += e.found ? "OK\n" : "NONE\n";
  out += std::to_string(off);
  out += '\n';
  out += std::to_string(body.size());
  out += '\n';
  out += body;
  return out;
}

bool ParseCacheEntry(const std::string& raw, const std::string& key, CacheEntry* out) {
  size_t p = 0;
  std::string magic, k, status;
  if (!NextLine(raw, &p, &magic) || !NextLine(raw, &p, &k) || k != key || !NextLine(raw, &p, &status)) return false;

  CacheEntry e;
  if (magic == kMagicV1) {
    if (status == "NONE") {
      e.found = false;
    } else if (status == "OK") {
      e.lrc = raw.substr(p);
      if (e.lrc.empty()) return false;
      e.found = true;
    } else {
      return false;
    }
  } else if (magic == kMagicV2) {
    std::string off_s, len_s;
    if (!NextLine(raw, &p, &off_s) || !NextLine(raw, &p, &len_s)) return false;
    long long off = 0, len = 0;
    if (!ParseInt(off_s, -kMaxTrackOffsetMs, kMaxTrackOffsetMs, &off)) return false;
    if (!ParseInt(len_s, 0, static_cast<long long>(raw.size()), &len)) return false;
    if (raw.size() - p != static_cast<size_t>(len)) return false;   // truncated (or padded) file
    if (status == "NONE") {
      if (len != 0) return false;
      e.found = false;
    } else if (status == "OK") {
      if (len == 0) return false;
      e.found = true;
      e.offset_ms = static_cast<int>(off);
      e.lrc = raw.substr(p);
    } else {
      return false;
    }
  } else {
    return false;
  }
  *out = std::move(e);
  return true;
}

std::vector<size_t> PickEvictions(const std::vector<CacheFileInfo>& files, size_t max_files, uint64_t max_bytes) {
  std::vector<size_t> victims;
  uint64_t total = 0;
  for (const auto& f : files) total += f.size;
  size_t count = files.size();
  if (count <= max_files && total <= max_bytes) return victims;

  std::vector<size_t> order(files.size());
  std::iota(order.begin(), order.end(), size_t{ 0 });
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {   // oldest first, path as a stable tie-break
    if (files[a].written != files[b].written) return files[a].written < files[b].written;
    return files[a].path < files[b].path;
  });
  for (size_t i : order) {
    if (count <= max_files && total <= max_bytes) break;
    victims.push_back(i);
    --count;
    total -= (std::min)(total, files[i].size);
  }
  return victims;
}

} // namespace island
