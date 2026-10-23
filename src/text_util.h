#pragma once
#include "common.h"

namespace ep {

// Radio metadata is UTF-8 or Latin-1 depending on the station: try UTF-8 first, fall back to Windows-1252.
std::wstring ToWide(const std::string& s);
std::string ToUtf8(const std::wstring& s);

// Splits a stream title like "Artist - Song" at the first dash. Stations use all kinds of look-alike
// dashes and spaces (en dash, U+02D7, no-break space, ...), which are treated like " - ".
// Returns false when there is no separator; `title` then holds the whole text.
bool SplitArtistTitle(const std::string& raw, std::wstring& artist, std::wstring& title);

// Helpers for matching names between a radio stream and an online catalogue.
std::wstring NormalizeKey(const std::wstring& s);     // lower case, letters and digits only
std::wstring StripBrackets(const std::wstring& s);    // "Song (feat. X) [Live]" -> "Song "
std::wstring FirstArtist(const std::wstring& s);      // "A & B feat. C" -> "A"
std::wstring BaseTitle(const std::wstring& s);        // "Song (Radio Edit) - Remastered" -> "Song"
bool SimilarKeys(const std::wstring& a, const std::wstring& b);  // one contains the other (both non-empty)
std::string UrlEncode(const std::string& utf8);

// Value of `"key":"..."` inside a JSON fragment (handles the usual escapes). No full JSON parser needed here.
bool JsonString(const std::string& obj, const char* key, std::string& out);

}  // namespace ep
