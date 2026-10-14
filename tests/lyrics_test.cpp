// Host-side unit tests for the pure lyrics code (LRC parser, JSON, cleaning, ranking).
//   g++ -std=c++20 -Isrc tests/lyrics_test.cpp src/lyrics/lrc.cpp src/lyrics/lyrics_text.cpp src/lyrics/lyrics_cache.cpp -o lyrics_test && ./lyrics_test
// (also builds with MSVC / mingw; nothing here touches Win32)

#include <cstdio>
#include <string>
#include <vector>

#include "lyrics/lrc.hpp"
#include "lyrics/lyrics_cache.hpp"
#include "lyrics/lyrics_text.hpp"

using namespace island;

static int g_fail = 0;
static int g_run = 0;

#define CHECK(cond)                                                                    \
  do {                                                                                 \
    ++g_run;                                                                           \
    if (!(cond)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #cond); }     \
  } while (0)

static void CheckW(int line, const std::wstring& got, const std::wstring& want) {
  ++g_run;
  if (got != want) {
    ++g_fail;
    std::printf("FAIL line %d: got \"%s\" want \"%s\"\n", line, WideToUtf8(got).c_str(), WideToUtf8(want).c_str());
  }
}
#define CHECKW(got, want) CheckW(__LINE__, (got), (want))

// FindLyricLineHinted must always agree with the plain binary search, whatever the hint is.
static void TestHintedFind() {
  std::vector<LyricLine> v;
  for (int i = 0; i < 40; ++i) v.push_back(LyricLine{ static_cast<int64_t>(1000 + i * 1500 + (i % 3) * 7), L"x" });
  v.push_back(LyricLine{ v.back().ms, L"dup" });   // equal timestamps
  bool same = true;
  for (int hint = -3; hint <= static_cast<int>(v.size()) + 2; ++hint)
    for (int64_t pos = -10; pos < 65000; pos += 97)
      if (FindLyricLineHinted(v, pos, hint) != FindLyricLine(v, pos)) same = false;
  CHECK(same);
  CHECK(FindLyricLineHinted({}, 500, -1) == -1);
  // sequential playback never needs the fallback and still matches
  int h = -1;
  bool seq = true;
  for (int64_t pos = 0; pos < 65000; pos += 16) {
    h = FindLyricLineHinted(v, pos, h);
    if (h != FindLyricLine(v, pos)) seq = false;
  }
  CHECK(seq);
}


// ---- disk cache: file format + eviction ------------------------------------------------------------------
static void TestCache() {
  const std::string key = "queen|bohemian rhapsody|355";
  const std::string lrc = "[00:01.00]Is this the real life?\r\n[00:05.00]Is this just fantasy?\n";

  // v2 round trip, including the per-track nudge and CR/LF inside the body (the length counts bytes, no translation)
  {
    CacheEntry e; e.found = true; e.offset_ms = -300; e.lrc = lrc;
    const std::string raw = SerializeCacheEntry(key, e);
    CHECK(raw.rfind("island-lyrics-2\n", 0) == 0);
    CacheEntry back;
    CHECK(ParseCacheEntry(raw, key, &back));
    CHECK(back.found && back.offset_ms == -300 && back.lrc == lrc);
    CHECK(!ParseCacheEntry(raw, "queen|other|355", &back));                 // another key (hash collision) is a miss
  }
  // negative entry
  {
    CacheEntry e; e.found = false; e.offset_ms = 500; e.lrc = "ignored";
    const std::string raw = SerializeCacheEntry(key, e);
    CacheEntry back; back.found = true;
    CHECK(ParseCacheEntry(raw, key, &back));
    CHECK(!back.found && back.offset_ms == 0 && back.lrc.empty());
  }
  // offset is clamped on write, rejected when out of range on read
  {
    CacheEntry e; e.found = true; e.offset_ms = 99999; e.lrc = lrc;
    CacheEntry back;
    CHECK(ParseCacheEntry(SerializeCacheEntry(key, e), key, &back) && back.offset_ms == kMaxTrackOffsetMs);
    const std::string bad = "island-lyrics-2\n" + key + "\nOK\n99999\n" + std::to_string(lrc.size()) + "\n" + lrc;
    CHECK(!ParseCacheEntry(bad, key, &back));
  }
  // truncated / padded / garbage files are rejected, never half-read
  {
    CacheEntry e; e.found = true; e.lrc = lrc;
    const std::string raw = SerializeCacheEntry(key, e);
    CacheEntry back;
    bool any_ok = false;
    for (size_t n = 0; n < raw.size(); ++n)                                  // every strict prefix
      if (ParseCacheEntry(raw.substr(0, n), key, &back)) any_ok = true;
    CHECK(!any_ok);
    CHECK(!ParseCacheEntry(raw + "x", key, &back));                          // longer than the stored length
    CHECK(!ParseCacheEntry("", key, &back));
    CHECK(!ParseCacheEntry("island-lyrics-9\n" + key + "\nOK\n0\n1\nx", key, &back));
    CHECK(!ParseCacheEntry("island-lyrics-2\n" + key + "\nMAYBE\n0\n3\nabc", key, &back));
    CHECK(!ParseCacheEntry("island-lyrics-2\n" + key + "\nOK\nabc\n3\nabc", key, &back));
    CHECK(!ParseCacheEntry("island-lyrics-2\n" + key + "\nOK\n0\n-1\nabc", key, &back));
    CHECK(!ParseCacheEntry("island-lyrics-2\n" + key + "\nOK\n0\n0\n", key, &back));        // OK without text
    CHECK(!ParseCacheEntry("island-lyrics-2\n" + key + "\nNONE\n0\n3\nabc", key, &back));    // NONE with text
    CHECK(!ParseCacheEntry("island-lyrics-2\n" + key + "\nOK\n0\n99999999999999999999\nabc", key, &back));
  }
  // a key with a line break cannot be stored
  {
    CacheEntry e; e.found = true; e.lrc = lrc;
    CHECK(SerializeCacheEntry("a\nb", e).empty());
  }
  // v1 files (the previous format) are still read
  {
    CacheEntry back;
    CHECK(ParseCacheEntry("island-lyrics-1\n" + key + "\nOK\n" + lrc, key, &back) && back.found && back.lrc == lrc &&
          back.offset_ms == 0);
    CHECK(ParseCacheEntry("island-lyrics-1\r\n" + key + "\r\nNONE\r\n", key, &back) && !back.found);
    CHECK(!ParseCacheEntry("island-lyrics-1\n" + key + "\nOK\n", key, &back));              // OK without text
  }
  // eviction: oldest first, until both limits hold
  {
    std::vector<CacheFileInfo> f;
    auto add = [&](const wchar_t* name, uint64_t size, uint64_t written) {
      CacheFileInfo i; i.path = name; i.size = size; i.written = written; f.push_back(i);
    };
    add(L"c", 100, 30); add(L"a", 100, 10); add(L"d", 100, 40); add(L"b", 100, 20);
    CHECK(PickEvictions(f, 10, 1000).empty());                               // within limits: nothing
    CHECK(PickEvictions(f, 4, 400).empty());                                 // exactly at the limits: nothing
    auto v = PickEvictions(f, 2, 1000);                                      // too many files: the two oldest (a, b)
    CHECK(v.size() == 2 && f[v[0]].path == L"a" && f[v[1]].path == L"b");
    v = PickEvictions(f, 10, 250);                                           // too many bytes: 4 -> 2 files kept
    CHECK(v.size() == 2 && f[v[0]].path == L"a" && f[v[1]].path == L"b");
    v = PickEvictions(f, 3, 1000);
    CHECK(v.size() == 1 && f[v[0]].path == L"a");
    v = PickEvictions(f, 0, 0);                                              // everything
    CHECK(v.size() == 4);
    add(L"e", 100, 20);                                                      // same time as b: path breaks the tie
    v = PickEvictions(f, 3, 1000);
    CHECK(v.size() == 2 && f[v[0]].path == L"a" && f[v[1]].path == L"b");
    CHECK(PickEvictions({}, 5, 5).empty());
  }
}

int main() {
  TestHintedFind();
  // ---- LRC ----
  {
    auto l = ParseLrc(L"[ar:Someone]\r\n[ti:Song]\r\n[00:12.00]Hello\r\n[00:15.50] World \r\n[01:02.345]End\r\n");
    CHECK(l.size() == 3);
    CHECK(l[0].ms == 12000 && l[0].text == L"Hello");
    CHECK(l[1].ms == 15500 && l[1].text == L"World");
    CHECK(l[2].ms == 62345 && l[2].text == L"End");
  }
  {
    auto l = ParseLrc(L"[00:10.00][01:20.00]chorus\n[00:30.00]verse\n");
    CHECK(l.size() == 3);
    CHECK(l[0].ms == 10000 && l[0].text == L"chorus");
    CHECK(l[1].ms == 30000 && l[1].text == L"verse");
    CHECK(l[2].ms == 80000 && l[2].text == L"chorus");
  }
  {
    // empty lines: leading one dropped, runs merged, one kept as a gap marker
    auto l = ParseLrc(L"[00:01.00]\n[00:05.00]a\n[00:09.00]\n[00:10.00]\n[00:20.00]b\n");
    CHECK(l.size() == 3);
    CHECK(l[0].text == L"a" && l[1].text.empty() && l[1].ms == 9000 && l[2].text == L"b");
  }
  {
    auto l = ParseLrc(L"[offset:+500]\n[00:10.00]x\n");
    CHECK(l.size() == 1 && l[0].ms == 9500);   // positive offset = earlier
    auto m = ParseLrc(L"[offset:-250]\n[00:10.00]x\n");
    CHECK(m.size() == 1 && m[0].ms == 10250);
  }
  {
    auto l = ParseLrc(L"[00:10.00]<00:10.00>Hel<00:10.40>lo <00:11.00>world\n");
    CHECK(l.size() == 1 && l[0].text == L"Hello world");
  }
  {
    auto l = ParseLrc(L"[00:10.5]a\n[00:11:25]b\n[1:02]c\n[99:99.99]d\nno tag line\n[xx:yy]bad\n");
    CHECK(l.size() == 4);
    CHECK(l[0].ms == 10500 && l[1].ms == 11250 && l[2].ms == 62000);
  }
  {
    auto l = ParseLrc(L"[00:30.00]late\n[00:10.00]early\n");
    CHECK(l.size() == 2 && l[0].text == L"early");
    CHECK(ParseLrc(L"").empty());
    CHECK(ParseLrc(L"just text\nno stamps").empty());
  }
  {
    auto l = ParseLrc(L"[00:10.00]a\n[00:20.00]b\n[00:30.00]c\n");
    CHECK(FindLyricLine(l, 0) == -1);
    CHECK(FindLyricLine(l, 9999) == -1);
    CHECK(FindLyricLine(l, 10000) == 0);
    CHECK(FindLyricLine(l, 19999) == 0);
    CHECK(FindLyricLine(l, 20000) == 1);
    CHECK(FindLyricLine(l, 999999) == 2);
    CHECK(FindLyricLine({}, 5) == -1);
  }

  // ---- cleaning ----
  CHECKW(CleanTitle(L"Bohemian Rhapsody - Remastered 2011"), L"Bohemian Rhapsody");
  CHECKW(CleanTitle(L"Song (Official Video)"), L"Song");
  CHECKW(CleanTitle(L"Song [HD] (Official Music Video)"), L"Song");
  CHECKW(CleanTitle(L"Song (feat. Someone)"), L"Song");
  CHECKW(CleanTitle(L"Song feat. Someone"), L"Song");
  CHECKW(CleanTitle(L"Song (Acoustic)"), L"Song (Acoustic)");
  CHECKW(CleanTitle(L"Shades of Cool"), L"Shades of Cool");
  CHECKW(CleanTitle(L"Song - Radio Edit"), L"Song - Radio Edit");
  CHECKW(CleanTitle(L"Artist - Song | Channel"), L"Artist - Song");
  CHECKW(CleanTitle(L"Hold On (with Brandon)"), L"Hold On");
  CHECKW(CleanTitle(L"Song  (Lyric Video)  "), L"Song");
  CHECKW(CleanTitle(L"Song - 2011 Remaster"), L"Song");
  CHECKW(CleanTitle(L"Dance with Me"), L"Dance with Me");
  CHECKW(CleanTitle(L"(Official Video)"), L"(Official Video)");   // never returns empty
  CHECKW(CleanArtist(L"Queen - Topic"), L"Queen");
  CHECKW(CleanArtist(L"ColdplayVEVO"), L"Coldplay");
  CHECKW(CleanArtist(L"A feat. B"), L"A");
  CHECKW(CleanArtist(L"  Normal Artist "), L"Normal Artist");
  CHECKW(FirstArtist(L"A, B & C"), L"A");
  CHECKW(FirstArtist(L"Simon & Garfunkel"), L"Simon");
  CHECKW(FirstArtist(L"Solo"), L"Solo");
  CHECKW(NormalizeName(L"Ke$ha - It's Me!"), L"keha" L"itsme");

  // ---- UTF-8 ----
  {
    const std::wstring w = L"café 日本語 \U0001F3B5";
    CHECKW(Utf8ToWide(WideToUtf8(w)), w);
    CHECK(WideToUtf8(L"é") == "\xC3\xA9");
    CHECK(UrlEncode("a b&c/\xC3\xA9") == "a%20b%26c%2F%C3%A9");
  }

  // ---- JSON ----
  {
    Json j;
    CHECK(ParseJson("{\"a\":1.5,\"b\":\"x\\u00e9\\n\\ud83c\\udfb5\",\"c\":[true,false,null,{\"d\":-2e1}],\"e\":{}}", &j));
    CHECK(j.Num("a") == 1.5);
    CHECK(j.Str("b") == "x\xC3\xA9\n\xF0\x9F\x8E\xB5");
    const Json* c = j.Get("c");
    CHECK(c && c->items.size() == 4 && c->items[0].b && !c->items[1].b && c->items[3].Num("d") == -20.0);
    CHECK(j.Get("zzz") == nullptr && j.Str("zzz").empty());
    CHECK(!ParseJson("{\"a\":}", &j));
    CHECK(!ParseJson("{\"a\":1} trailing", &j));
    CHECK(!ParseJson("[1,2", &j));
    CHECK(!ParseJson("", &j));
    CHECK(ParseJson("  [ ]  ", &j) && j.items.empty());
    std::string deep(100, '[');
    CHECK(!ParseJson(deep, &j));
  }

  // ---- ranking ----
  {
    std::vector<LyricsCandidate> v;
    auto add = [&](const char* t, const char* a, double d, const char* synced, bool inst = false) {
      LyricsCandidate c; c.track = t; c.artist = a; c.duration = d; c.synced = synced; c.instrumental = inst;
      v.push_back(c);
    };
    add("Bohemian Rhapsody", "Queen", 355, "[00:01.00]x");                      // 0 exact
    add("Bohemian Rhapsody", "Some Cover Band", 355, "[00:01.00]x");            // 1 wrong artist
    add("Bohemian Rhapsody", "Queen", 340, "[00:01.00]x");                      // 2 wrong length
    add("Bohemian Rhapsody (Live)", "Queen", 356, "[00:01.00]x");               // 3 live
    add("Bohemian Rhapsody", "Queen", 355, "");                                 // 4 no synced
    CHECK(PickBestCandidate(v, L"Bohemian Rhapsody", L"Queen", 355) == 0);
    CHECK(PickBestCandidate(v, L"Bohemian Rhapsody", L"Queen", 356) == 0);      // 355 vs 356 beats 356 live (title differs)
    CHECK(PickBestCandidate(v, L"Bohemian Rhapsody", L"Queen", 200) == -1);     // nothing within 6 s
    CHECK(PickBestCandidate(v, L"Bohemian Rhapsody", L"Queen", 0) == 0);        // unknown length: best title/artist
    CHECK(PickBestCandidate(v, L"Other Song", L"Queen", 355) == -1);
    CHECK(PickBestCandidate(v, L"Bohemian Rhapsody", L"Nobody Known", 355) == -1);
    CHECK(PickBestCandidate(v, L"Bohemian Rhapsody", L"", 355) == 0);           // artist unknown: accept
    std::vector<LyricsCandidate> w;
    LyricsCandidate c; c.track = "Hello"; c.artist = "Mahalia Jackson"; c.duration = 200; c.synced = "[00:01.00]a";
    w.push_back(c);
    CHECK(PickBestCandidate(w, L"Hello", L"Mahalia", 201) == 0);                // partial artist
    CHECK(PickBestCandidate(w, L"Hello", L"Jackson Browne", 201) == 0);         // shared word, tolerated
    CHECK(PickBestCandidate(w, L"Hello", L"Adele", 201) == -1);

    Json j;
    CHECK(ParseJson("{\"trackName\":\"T\",\"artistName\":\"A\",\"albumName\":\"B\",\"duration\":123.4,"
                    "\"instrumental\":false,\"plainLyrics\":\"p\",\"syncedLyrics\":\"[00:01.00]x\"}", &j));
    LyricsCandidate cj;
    CHECK(CandidateFromJson(j, &cj) && cj.track == "T" && cj.artist == "A" && cj.duration == 123.4 && !cj.synced.empty());
  }

  TestCache();

  std::printf("%d checks, %d failed\n", g_run, g_fail);
  return g_fail ? 1 : 0;
}
