#pragma once
// Lyrics – pure text helpers (no Win32): UTF-8 <-> wide, title/artist cleaning, a tiny JSON reader,
// LRCLIB result ranking, URL encoding, cache-key hashing. Unit-tested on the host (tests/lyrics_test.cpp).

#include <cstdint>
#include <string>
#include <vector>

namespace island {

std::wstring Utf8ToWide(const std::string& s);   // wchar_t is UTF-16 on Windows, UTF-32 elsewhere
std::string WideToUtf8(const std::wstring& s);
std::string UrlEncode(const std::string& utf8);
std::wstring TrimSpace(const std::wstring& s);

// "Song (Official Video) [HD] feat. X" -> "Song"; "Song - Remastered 2011" -> "Song"; "A | Channel" -> "A".
// Meaningful suffixes such as "(Acoustic)", "(Live)" or "- Radio Edit" are kept.
std::wstring CleanTitle(const std::wstring& title);
// "Artist - Topic" -> "Artist"; "ArtistVEVO" -> "Artist"; "A feat. B" -> "A".
std::wstring CleanArtist(const std::wstring& artist);
// "A, B & C" -> "A" (for a second lookup attempt); returns the input when there is no separator.
std::wstring FirstArtist(const std::wstring& artist);
// Lower-case letters and digits only, for comparing names.
std::wstring NormalizeName(const std::wstring& s);

uint64_t Fnv1a64(const std::string& s);
std::string Hex64(uint64_t v);

// ---- minimal JSON (enough for LRCLIB) ---------------------------------------------------------
struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool b = false;
  double n = 0.0;
  std::string s;                    // UTF-8
  std::vector<Json> items;          // Array
  std::vector<std::string> keys;    // Object (parallel to `items`)

  const Json* Get(const char* key) const;
  std::string Str(const char* key) const;            // "" when missing / not a string
  double Num(const char* key, double def = 0.0) const;
  bool Bool(const char* key, bool def = false) const;
};
bool ParseJson(const std::string& text, Json* out);   // false on any syntax error / depth > 32

// ---- LRCLIB candidates -------------------------------------------------------------------------
struct LyricsCandidate {
  std::string track, artist, album;   // UTF-8
  double duration = 0.0;              // seconds, 0 = unknown
  std::string synced;                 // LRC text, may be empty
  bool instrumental = false;
};

// Reads one LRCLIB record ({"trackName":..,"artistName":..,"duration":..,"syncedLyrics":..,"instrumental":..}).
bool CandidateFromJson(const Json& j, LyricsCandidate* out);

// Picks the best synced-lyrics match for the wanted track out of `list`; -1 when nothing is good enough.
// Rejects other titles, other artists (when the artist is known) and durations more than ~6 s apart.
int PickBestCandidate(const std::vector<LyricsCandidate>& list, const std::wstring& title,
                      const std::wstring& artist, int duration_sec);

} // namespace island
