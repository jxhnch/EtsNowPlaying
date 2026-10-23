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

}  // namespace ep
