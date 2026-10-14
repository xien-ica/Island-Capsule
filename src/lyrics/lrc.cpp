// LRC parser (platform independent).

#include "lyrics/lrc.hpp"

#include <algorithm>
#include <cwctype>
#include <string_view>

namespace island {

namespace {

bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

// Parses "mm:ss", "mm:ss.xx", "mm:ss.xxx", "mm:ss:xx" (the text between the brackets).
bool ParseStamp(std::wstring_view s, int64_t* out) {
  size_t i = 0;
  int64_t mm = 0;
  size_t digits = 0;
  while (i < s.size() && IsDigit(s[i]) && digits < 6) { mm = mm * 10 + (s[i] - L'0'); ++i; ++digits; }
  if (digits == 0 || i >= s.size() || s[i] != L':') return false;
  ++i;
  int64_t ss = 0;
  digits = 0;
  while (i < s.size() && IsDigit(s[i]) && digits < 3) { ss = ss * 10 + (s[i] - L'0'); ++i; ++digits; }
  if (digits == 0) return false;
  int64_t frac_ms = 0;
  if (i < s.size()) {
    if (s[i] != L'.' && s[i] != L':') return false;
    ++i;
    int64_t scale = 100;   // first fractional digit = tenths of a second
    digits = 0;
    while (i < s.size() && IsDigit(s[i])) {
      if (digits < 3) { frac_ms += (s[i] - L'0') * scale; scale /= 10; }
      ++i;
      ++digits;
    }
    if (digits == 0) return false;
  }
  if (i != s.size()) return false;
  *out = (mm * 60 + ss) * 1000 + frac_ms;
  return true;
}

std::wstring_view Trim(std::wstring_view s) {
  size_t a = 0, b = s.size();
  while (a < b && (std::iswspace(s[a]) || s[a] == 0xFEFF)) ++a;
  while (b > a && std::iswspace(s[b - 1])) --b;
  return s.substr(a, b - a);
}

// Trimmed text of a lyric line with enhanced-LRC word timestamps like <00:12.34> removed.
// One allocation (the result); a view is only copied when the line is actually kept.
std::wstring CleanBody(std::wstring_view s) {
  if (s.find(L'<') == std::wstring_view::npos) return std::wstring(Trim(s));
  std::wstring out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == L'<') {
      const size_t e = s.find(L'>', i);
      int64_t dummy = 0;
      if (e != std::wstring_view::npos && ParseStamp(s.substr(i + 1, e - i - 1), &dummy)) {
        i = e + 1;
        continue;
      }
    }
    out.push_back(s[i++]);
  }
  const std::wstring_view t = Trim(out);
  if (t.size() != out.size()) return std::wstring(t);
  return out;
}

} // namespace

std::vector<LyricLine> ParseLrc(const std::wstring& text) {
  std::vector<LyricLine> out;
  int64_t offset_ms = 0;

  const std::wstring_view all(text);
  out.reserve(static_cast<size_t>(std::count(all.begin(), all.end(), L'\n')) + 1);

  std::vector<int64_t> stamps;   // reused for every line
  size_t pos = 0;
  while (pos <= all.size()) {
    size_t nl = all.find(L'\n', pos);
    if (nl == std::wstring_view::npos) nl = all.size();
    std::wstring_view line = all.substr(pos, nl - pos);
    pos = nl + 1;
    if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);

    // leading [..] tags
    stamps.clear();
    size_t i = 0;
    while (i < line.size() && std::iswspace(line[i])) ++i;
    if (i < line.size() && line[i] == 0xFEFF) ++i;
    for (;;) {
      if (i >= line.size() || line[i] != L'[') break;
      const size_t close = line.find(L']', i);
      if (close == std::wstring_view::npos) break;
      const std::wstring_view tag = line.substr(i + 1, close - i - 1);
      int64_t ms = 0;
      if (ParseStamp(tag, &ms)) {
        stamps.push_back(ms);
      } else {
        // metadata tag: [offset:+500] shifts the lyrics, the others (ar, ti, al, by ...) are ignored
        const size_t colon = tag.find(L':');
        if (colon != std::wstring_view::npos && Trim(tag.substr(0, colon)) == L"offset") {
          const std::wstring_view v = Trim(tag.substr(colon + 1));
          size_t k = 0;
          bool neg = false;
          if (k < v.size() && (v[k] == L'+' || v[k] == L'-')) { neg = v[k] == L'-'; ++k; }
          int64_t n = 0;
          size_t d = 0;
          while (k < v.size() && IsDigit(v[k]) && d < 7) { n = n * 10 + (v[k] - L'0'); ++k; ++d; }
          if (d > 0) offset_ms = neg ? -n : n;
        }
      }
      i = close + 1;
    }
    if (stamps.empty()) continue;

    std::wstring body = CleanBody(line.substr(i));
    for (size_t k = 0; k + 1 < stamps.size(); ++k) out.push_back(LyricLine{ stamps[k], body });   // extra stamps copy...
    out.push_back(LyricLine{ stamps.back(), std::move(body) });                                  // ...the last one moves
  }

  // [offset:+N] means the lyrics should appear N ms EARLIER (LRC convention).
  if (offset_ms != 0)
    for (auto& l : out) l.ms = std::max<int64_t>(0, l.ms - offset_ms);

  // Lyrics are nearly always already in order: only sort when they are not.
  const auto by_time = [](const LyricLine& a, const LyricLine& b) { return a.ms < b.ms; };
  if (!std::is_sorted(out.begin(), out.end(), by_time)) std::stable_sort(out.begin(), out.end(), by_time);

  // merge consecutive empty lines, drop a leading empty line (in place)
  size_t w = 0;
  for (size_t r = 0; r < out.size(); ++r) {
    if (out[r].text.empty() && (w == 0 || out[w - 1].text.empty())) continue;
    if (w != r) out[w] = std::move(out[r]);
    ++w;
  }
  out.resize(w);
  out.shrink_to_fit();
  return out;
}

int FindLyricLine(const std::vector<LyricLine>& lines, int64_t pos_ms) {
  if (lines.empty() || pos_ms < lines.front().ms) return -1;
  size_t lo = 0, hi = lines.size();   // first index with ms > pos_ms
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (lines[mid].ms <= pos_ms) lo = mid + 1; else hi = mid;
  }
  return static_cast<int>(lo) - 1;
}

int FindLyricLineHinted(const std::vector<LyricLine>& lines, int64_t pos_ms, int hint) {
  const int n = static_cast<int>(lines.size());
  if (n == 0) return -1;
  // Playback moves forward one line at a time: check the previous answer and its successor first.
  if (hint >= -1 && hint < n) {
    for (int c = hint; c <= hint + 1 && c < n; ++c) {
      const bool after_start = c < 0 ? true : lines[static_cast<size_t>(c)].ms <= pos_ms;
      const bool before_next = c + 1 >= n ? true : pos_ms < lines[static_cast<size_t>(c + 1)].ms;
      const bool at_start = c >= 0 || pos_ms < lines[0].ms;
      if (after_start && before_next && at_start) return c;
    }
  }
  return FindLyricLine(lines, pos_ms);
}

} // namespace island
