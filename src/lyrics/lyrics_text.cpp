// Lyrics – pure text helpers.

#include "lyrics/lyrics_text.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>

namespace island {

// ---- UTF-8 -------------------------------------------------------------------------------------

std::wstring Utf8ToWide(const std::string& s) {
  std::wstring out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0xFFFD;
    int len = 1;
    if (c < 0x80) { cp = c; }
    else if ((c >> 5) == 0x6 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
    else if ((c >> 4) == 0xE && i + 2 < s.size()) {
      cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); len = 3;
    } else if ((c >> 3) == 0x1E && i + 3 < s.size()) {
      cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); len = 4;
    }
    i += len;
    if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
      cp -= 0x10000;
      out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<wchar_t>(cp));
    }
  }
  return out;
}

std::string WideToUtf8(const std::wstring& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t cp = static_cast<uint32_t>(s[i]);
    if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() &&
        static_cast<uint32_t>(s[i + 1]) >= 0xDC00 && static_cast<uint32_t>(s[i + 1]) < 0xE000) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint32_t>(s[i + 1]) - 0xDC00);
      ++i;
    }
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

std::string UrlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  return out;
}

// ---- cleaning ----------------------------------------------------------------------------------

std::wstring TrimSpace(const std::wstring& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::iswspace(s[a])) ++a;
  while (b > a && std::iswspace(s[b - 1])) --b;
  return s.substr(a, b - a);
}

namespace {

std::wstring Lower(std::wstring s) {
  for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
  return s;
}

std::wstring Trim(const std::wstring& s) { return TrimSpace(s); }

std::wstring Collapse(const std::wstring& s) {
  std::wstring out;
  bool sp = false;
  for (wchar_t c : s) {
    if (std::iswspace(c)) { sp = true; continue; }
    if (sp && !out.empty()) out.push_back(L' ');
    sp = false;
    out.push_back(c);
  }
  return out;
}

// True when `w` occurs in `l` as a whole word: a letter or digit right before or after the hit means it is part of a
// longer word ("Featherweight" must not match "feat", "Monologue" must not match "mono"). `stem` words may continue
// to the right ("remaster" -> "remastered", "lyric" -> "lyrics").
bool HasWord(const std::wstring& l, const std::wstring& w, bool stem) {
  for (size_t at = l.find(w); at != std::wstring::npos; at = l.find(w, at + 1)) {
    const size_t end = at + w.size();
    const bool left_ok = !std::iswalnum(w.front()) || at == 0 || !std::iswalnum(l[at - 1]);
    const bool right_ok = stem || !std::iswalnum(w.back()) || end >= l.size() || !std::iswalnum(l[end]);
    if (left_ok && right_ok) return true;
  }
  return false;
}

// Words that make a bracketed group / dash suffix pure video-or-release noise.
bool IsNoise(const std::wstring& text) {
  const std::wstring l = Trim(Lower(text));
  struct Word { const wchar_t* w; bool stem; };
  static const Word kWords[] = {
    { L"official", false }, { L"video", false }, { L"audio", false }, { L"lyric", true }, { L"visuali", true },
    { L"remaster", true }, { L"music video", false }, { L"explicit", false }, { L"clean version", false },
    { L"hd", false }, { L"hq", false }, { L"4k", false }, { L"1080p", false }, { L"m/v", false },
    { L"full song", false }, { L"full version", false }, { L"topic", false }, { L"feat", false },
    { L"ft.", false }, { L"featuring", false }, { L"prod.", false }, { L"bonus track", false },
    { L"deluxe", false }, { L"mono", false }, { L"stereo", false },
  };
  for (const Word& k : kWords)
    if (HasWord(l, k.w, k.stem)) return true;
  // "(with Brandon)" is a credit, but "with" inside a title ("Walk with Me") is not noise: only at the start.
  if (l.compare(0, 5, L"with ") == 0) return true;
  return l == L"mv";
}

bool IsOpen(wchar_t c) { return c == L'(' || c == L'[' || c == 0x3010 || c == 0xFF08; }
wchar_t CloserFor(wchar_t c) {
  switch (c) {
    case L'(': return L')';
    case L'[': return L']';
    case 0x3010: return 0x3011;
    default: return 0xFF09;
  }
}

// Cuts "feat. X" / "ft. X" / "featuring X" (not inside brackets) up to the next " - " or the end.
std::wstring CutFeat(const std::wstring& s) {
  const std::wstring l = Lower(s);
  static const wchar_t* kMarks[] = { L" feat. ", L" feat ", L" ft. ", L" ft ", L" featuring ", L" with " };
  size_t best = std::wstring::npos;
  for (const wchar_t* m : kMarks) {
    const size_t at = l.find(m);
    if (at != std::wstring::npos && at < best) best = at;
  }
  if (best == std::wstring::npos) return s;
  // " with " is only noise when something like "(with X)" - bare " with " is part of many titles
  if (l.compare(best, 6, L" with ") == 0) return s;
  size_t end = l.find(L" - ", best);
  std::wstring out = s.substr(0, best);
  if (end != std::wstring::npos) out += s.substr(end);
  return out;
}

} // namespace

std::wstring CleanTitle(const std::wstring& title) {
  // 1) drop bracketed groups that are noise
  std::wstring t;
  for (size_t i = 0; i < title.size();) {
    if (IsOpen(title[i])) {
      const wchar_t close = CloserFor(title[i]);
      const size_t e = title.find(close, i + 1);
      if (e != std::wstring::npos) {
        if (IsNoise(title.substr(i + 1, e - i - 1))) { i = e + 1; continue; }
      }
    }
    t.push_back(title[i++]);
  }

  // 2) "Song | Channel" -> "Song"
  const size_t bar = t.find(L" | ");
  if (bar != std::wstring::npos && bar > 0) t.resize(bar);

  // 3) " - Remastered 2011" / " - Official Video" style suffixes (only the LAST dash group)
  for (;;) {
    const size_t dash = t.rfind(L" - ");
    if (dash == std::wstring::npos || dash == 0) break;
    if (!IsNoise(t.substr(dash + 3))) break;
    t.resize(dash);
  }

  // 4) bare "feat. X"
  t = CutFeat(t);
  t = Trim(Collapse(t));
  return t.empty() ? Trim(title) : t;
}

std::wstring CleanArtist(const std::wstring& artist) {
  std::wstring a = Trim(artist);
  const std::wstring l = Lower(a);
  if (l.size() > 8 && l.compare(l.size() - 8, 8, L" - topic") == 0) a.resize(a.size() - 8);
  else if (l.size() > 4 && l.compare(l.size() - 4, 4, L"vevo") == 0) a.resize(a.size() - 4);
  a = CutFeat(a);
  return Trim(Collapse(a));
}

std::wstring FirstArtist(const std::wstring& artist) {
  static const wchar_t* kSeps[] = { L", ", L" & ", L" x ", L" and ", L" / ", L"; ", L" + " };
  size_t cut = std::wstring::npos;
  const std::wstring l = Lower(artist);
  for (const wchar_t* sp : kSeps) {
    const size_t at = l.find(sp);
    if (at != std::wstring::npos && at > 0 && at < cut) cut = at;
  }
  return cut == std::wstring::npos ? artist : Trim(artist.substr(0, cut));
}

std::wstring NormalizeName(const std::wstring& s) {
  std::wstring out;
  for (wchar_t c : s) {
    // iswalnum covers letters of every script on the CRT; anything >= 0x80 that is not punctuation is kept
    if (std::iswalnum(c) || (static_cast<uint32_t>(c) >= 0x3040 && !std::iswpunct(c) && !std::iswspace(c)))
      out.push_back(static_cast<wchar_t>(std::towlower(c)));
  }
  return out;
}

uint64_t Fnv1a64(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
  return h;
}

std::string Hex64(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

// ---- JSON --------------------------------------------------------------------------------------

const Json* Json::Get(const char* key) const {
  if (type != Type::Object) return nullptr;
  for (size_t i = 0; i < keys.size(); ++i)
    if (keys[i] == key) return &items[i];
  return nullptr;
}
std::string Json::Str(const char* key) const {
  const Json* j = Get(key);
  return (j && j->type == Type::String) ? j->s : std::string();
}
double Json::Num(const char* key, double def) const {
  const Json* j = Get(key);
  return (j && j->type == Type::Number) ? j->n : def;
}
bool Json::Bool(const char* key, bool def) const {
  const Json* j = Get(key);
  return (j && j->type == Type::Bool) ? j->b : def;
}

namespace {

struct JsonReader {
  const std::string& t;
  size_t i = 0;
  explicit JsonReader(const std::string& text) : t(text) {}

  void Ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i; }

  static void AppendUtf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o.push_back(static_cast<char>(cp));
    else if (cp < 0x800) { o.push_back(static_cast<char>(0xC0 | (cp >> 6))); o.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
      o.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      o.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      o.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool Hex4(uint32_t* v) {
    if (i + 4 > t.size()) return false;
    uint32_t r = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = t[i + k];
      r <<= 4;
      if (c >= '0' && c <= '9') r |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') r |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') r |= static_cast<uint32_t>(c - 'A' + 10);
      else return false;
    }
    i += 4;
    *v = r;
    return true;
  }

  bool String(std::string* out) {
    if (i >= t.size() || t[i] != '"') return false;
    ++i;
    out->clear();
    while (i < t.size()) {
      const char c = t[i++];
      if (c == '"') return true;
      if (c != '\\') { out->push_back(c); continue; }
      if (i >= t.size()) return false;
      const char e = t[i++];
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!Hex4(&cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') {
            const size_t save = i;
            i += 2;
            uint32_t lo = 0;
            if (Hex4(&lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            else i = save;
          }
          AppendUtf8(*out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool Value(Json* j, int depth) {
    if (depth > 32) return false;
    Ws();
    if (i >= t.size()) return false;
    const char c = t[i];
    if (c == '{') {
      ++i;
      j->type = Json::Type::Object;
      Ws();
      if (i < t.size() && t[i] == '}') { ++i; return true; }
      for (;;) {
        Ws();
        std::string key;
        if (!String(&key)) return false;
        Ws();
        if (i >= t.size() || t[i] != ':') return false;
        ++i;
        Json v;
        if (!Value(&v, depth + 1)) return false;
        j->keys.push_back(std::move(key));
        j->items.push_back(std::move(v));
        Ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == '}') { ++i; return true; }
        return false;
      }
    }
    if (c == '[') {
      ++i;
      j->type = Json::Type::Array;
      Ws();
      if (i < t.size() && t[i] == ']') { ++i; return true; }
      for (;;) {
        Json v;
        if (!Value(&v, depth + 1)) return false;
        j->items.push_back(std::move(v));
        Ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == ']') { ++i; return true; }
        return false;
      }
    }
    if (c == '"') { j->type = Json::Type::String; return String(&j->s); }
    if (t.compare(i, 4, "true") == 0) { i += 4; j->type = Json::Type::Bool; j->b = true; return true; }
    if (t.compare(i, 5, "false") == 0) { i += 5; j->type = Json::Type::Bool; j->b = false; return true; }
    if (t.compare(i, 4, "null") == 0) { i += 4; j->type = Json::Type::Null; return true; }
    // number
    const size_t st = i;
    while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '-' || t[i] == '+' ||
                            t[i] == '.' || t[i] == 'e' || t[i] == 'E'))
      ++i;
    if (i == st) return false;
    j->type = Json::Type::Number;
    j->n = std::strtod(t.substr(st, i - st).c_str(), nullptr);
    return true;
  }
};

bool Contains(const std::wstring& a, const std::wstring& b) {
  return !a.empty() && !b.empty() && (a.find(b) != std::wstring::npos || b.find(a) != std::wstring::npos);
}

// Any word (3+ letters) of `want` that appears in `have`: "Mahalia" ~ "Mahalia Jackson".
bool ArtistOverlap(const std::wstring& want_raw, const std::wstring& have_raw) {
  const std::wstring have = NormalizeName(have_raw);
  std::wstring word;
  auto hit = [&](const std::wstring& w) { return w.size() >= 3 && NormalizeName(w).size() >= 3 && have.find(NormalizeName(w)) != std::wstring::npos; };
  for (wchar_t c : want_raw + L" ") {
    if (std::iswspace(c) || c == L',' || c == L'&' || c == L'/' || c == L';') {
      if (hit(word)) return true;
      word.clear();
    } else {
      word.push_back(c);
    }
  }
  return false;
}

} // namespace

bool ParseJson(const std::string& text, Json* out) {
  JsonReader r(text);
  Json v;
  if (!r.Value(&v, 0)) return false;
  r.Ws();
  if (r.i != text.size()) return false;
  *out = std::move(v);
  return true;
}

bool CandidateFromJson(const Json& j, LyricsCandidate* out) {
  if (j.type != Json::Type::Object) return false;
  LyricsCandidate c;
  c.track = j.Str("trackName");
  if (c.track.empty()) c.track = j.Str("name");
  c.artist = j.Str("artistName");
  c.album = j.Str("albumName");
  c.duration = j.Num("duration", 0.0);
  c.synced = j.Str("syncedLyrics");
  c.instrumental = j.Bool("instrumental", false);
  *out = std::move(c);
  return true;
}

int PickBestCandidate(const std::vector<LyricsCandidate>& list, const std::wstring& title,
                      const std::wstring& artist, int duration_sec) {
  const std::wstring want_title = NormalizeName(title);
  const std::wstring want_artist = NormalizeName(artist);
  int best = -1;
  double best_score = 1e9;
  for (size_t i = 0; i < list.size(); ++i) {
    const LyricsCandidate& c = list[i];
    if (c.instrumental || c.synced.empty()) continue;

    const std::wstring ct = NormalizeName(Utf8ToWide(c.track));
    double score = 0.0;
    if (ct == want_title) score += 0.0;
    else if (Contains(ct, want_title)) score += 20.0;
    else continue;

    if (!want_artist.empty()) {
      const std::wstring ca = NormalizeName(Utf8ToWide(c.artist));
      if (ca == want_artist) score += 0.0;
      else if (Contains(ca, want_artist)) score += 5.0;
      else if (ArtistOverlap(artist, Utf8ToWide(c.artist)) || ArtistOverlap(Utf8ToWide(c.artist), artist)) score += 10.0;
      else continue;
    }

    if (duration_sec > 0 && c.duration > 0.0) {
      const double diff = std::fabs(c.duration - duration_sec);
      if (diff > 6.0) continue;
      score += diff * 2.0;
    }
    if (score < best_score) { best_score = score; best = static_cast<int>(i); }
  }
  return best;
}

} // namespace island
