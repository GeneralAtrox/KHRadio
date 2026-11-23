// KH Radio - sharing a song (see ShareLink.h). GPL-3.0-or-later (see COPYING.txt).
#include "stdafx.h"
#include "ShareLink.h"
#include "TextUtil.h"

#include <cwctype>

namespace KHShare
{
	namespace
	{
		const wchar_t kHost[] = L"downloads.khinsider.com";
		const wchar_t kAlbumPath[] = L"/game-soundtracks/album/";
		const wchar_t kAlbumBase[] = L"https://downloads.khinsider.com/game-soundtracks/album/";
		constexpr int kMaxText = 65536;

		int HexValue(char c)
		{
			if (c >= '0' && c <= '9') { return c - '0'; }
			if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
			if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
			return -1;
		}

		// One round of %XX decoding (as UTF-8). Returns false if nothing changed.
		bool DecodeOnce(CStringW& s)
		{
			if (s.Find(L'%') == -1) {
				return false;
			}
			const CStringA utf8 = WStrToUTF8(s);
			std::string out;
			out.reserve(static_cast<size_t>(utf8.GetLength()));
			bool changed = false;
			for (int i = 0; i < utf8.GetLength(); i++) {
				const char c = utf8[i];
				if (c == '%' && i + 2 < utf8.GetLength() && HexValue(utf8[i + 1]) >= 0 && HexValue(utf8[i + 2]) >= 0) {
					out += static_cast<char>(HexValue(utf8[i + 1]) * 16 + HexValue(utf8[i + 2]));
					i += 2;
					changed = true;
				} else {
					out += c;
				}
			}
			if (changed) {
				s = UTF8ToWStr(out.c_str());
			}
			return changed;
		}

		// KHInsider's own track links are encoded twice ("%2520"); a chat app may
		// have decoded one level, or none. Decode until nothing is left (bounded).
		CStringW FullyDecoded(CStringW s)
		{
			for (int round = 0; round < 4 && DecodeOnce(s); round++) {
			}
			return s;
		}

		bool StartsWithNoCase(const CStringW& s, int at, LPCWSTR prefix)
		{
			const int n = static_cast<int>(wcslen(prefix));
			return at + n <= s.GetLength() && _wcsnicmp(s.GetString() + at, prefix, static_cast<size_t>(n)) == 0;
		}

		bool IsSlugChar(wchar_t c)
		{
			return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')
				|| c == L'-' || c == L'_' || c == L'.' || c == L'~';
		}

		// Characters that may sit right before an address inside a longer one; a
		// candidate preceded by one of these is part of something else.
		bool IsUrlChar(wchar_t c)
		{
			return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')
				|| wcschr(L".-~/=%?&@+#;$", c) != nullptr;
		}

		bool EndsCandidate(wchar_t c)
		{
			return c < 0x20 || iswspace(c) || c == 0xA0 || c == 0x200B || c == 0x3000
				|| wcschr(L"<>\"`|{}^\\", c) != nullptr;
		}

		CStringW OneLine(CStringW s)
		{
			s.Replace(L'\r', L' ');
			s.Replace(L'\n', L' ');
			s.Trim();
			return s;
		}
	}

	bool ParseLink(const CStringW& url, Link& link)
	{
		link = Link();
		int pos = 0;
		if (StartsWithNoCase(url, 0, L"https://")) {
			pos = 8;
		} else if (StartsWithNoCase(url, 0, L"http://")) {
			pos = 7;
		} else if (!StartsWithNoCase(url, 0, L"downloads.") && !StartsWithNoCase(url, 0, L"www.downloads.")) {
			return false; // another scheme, or not an address at all
		}
		int hostEnd = pos;
		while (hostEnd < url.GetLength() && wcschr(L"/?#", url[hostEnd]) == nullptr) {
			hostEnd++;
		}
		CStringW host = url.Mid(pos, hostEnd - pos);
		host.MakeLower();
		if (host.Left(4) == L"www.") {
			host = host.Mid(4);
		}
		if (host != kHost) {
			return false; // lookalikes, user info ("x@evil"), ports and odd separators all land here
		}

		CStringW path = url.Mid(hostEnd);
		const int query = path.FindOneOf(L"?#");
		if (query != -1) {
			path.Truncate(query);
		}
		if (!StartsWithNoCase(path, 0, kAlbumPath) || path.Find(L'\\') != -1) {
			return false;
		}
		CStringW rest = path.Mid(static_cast<int>(wcslen(kAlbumPath)));
		if (rest.Right(1) == L"/") {
			rest.Truncate(rest.GetLength() - 1);
		}
		std::vector<CStringW> parts;
		int start = 0;
		while (start <= rest.GetLength()) {
			int slash = rest.Find(L'/', start);
			if (slash == -1) {
				slash = rest.GetLength();
			}
			parts.push_back(rest.Mid(start, slash - start));
			start = slash + 1;
		}
		if (parts.empty() || parts.size() > 2) {
			return false;
		}
		const CStringW slug = FullyDecoded(parts[0]);
		if (slug.IsEmpty() || slug.GetLength() > 200 || slug == L"." || slug == L"..") {
			return false;
		}
		for (int i = 0; i < slug.GetLength(); i++) {
			if (!IsSlugChar(slug[i])) {
				return false;
			}
		}
		link.albumUrl = CStringW(kAlbumBase) + slug;
		if (parts.size() == 1) {
			link.kind = LinkKind::Album;
			return true;
		}
		CStringW key = FullyDecoded(parts[1]);
		key.MakeLower();
		if (key.IsEmpty() || key == L"." || key == L"..") {
			link = Link();
			return false;
		}
		link.kind = LinkKind::Track;
		link.trackKey = key;
		return true;
	}

	bool FindLink(const CStringW& textIn, Link& link)
	{
		link = Link();
		const CStringW text = textIn.GetLength() > kMaxText ? textIn.Left(kMaxText) : textIn;
		const int n = text.GetLength();
		for (int p = 0; p < n; p++) {
			if (!StartsWithNoCase(text, p, L"http://") && !StartsWithNoCase(text, p, L"https://")
					&& !StartsWithNoCase(text, p, L"downloads.khinsider.com") && !StartsWithNoCase(text, p, L"www.downloads.khinsider.com")) {
				continue;
			}
			if (p > 0 && IsUrlChar(text[p - 1])) {
				continue; // inside another address ("...?u=https://", "evil/downloads...")
			}
			int end = p;
			while (end < n && !EndsCandidate(text[end])) {
				end++;
			}
			CStringW candidate = text.Mid(p, end - p);
			// trailing sentence punctuation, markdown emphasis and unbalanced closers
			for (;;) {
				const int len = candidate.GetLength();
				if (len == 0) {
					break;
				}
				const wchar_t last = candidate[len - 1];
				const auto count = [&](wchar_t c) {
					int k = 0;
					for (int i = 0; i < len; i++) { k += candidate[i] == c ? 1 : 0; }
					return k;
				};
				const bool unbalanced = (last == L')' && count(L')') > count(L'('))
					|| (last == L']' && count(L']') > count(L'['));
				if (unbalanced || wcschr(L".,;:!?'*_\x2019\x201D\x3001\x3002\xFF01\xFF0C\xFF1F", last) != nullptr) {
					candidate.Truncate(len - 1);
				} else {
					break;
				}
			}
			// a markdown link's text may itself be the address: "[url](url)"
			const int md = candidate.Find(L"](");
			if (md != -1) {
				candidate.Truncate(md);
			}
			if (ParseLink(candidate, link)) {
				return true;
			}
		}
		link = Link();
		return false;
	}

	CStringW TrackKeyOf(const CStringW& trackPageUrl)
	{
		Link link;
		return ParseLink(trackPageUrl, link) && link.kind == LinkKind::Track ? link.trackKey : CStringW();
	}

	CStringW TrackPageFor(const CStringW& trackPageUrl, const CStringW& albumUrl, const CStringW& audioUrl)
	{
		if (!TrackKeyOf(trackPageUrl).IsEmpty()) {
			return trackPageUrl;
		}
		Link album;
		if (!ParseLink(albumUrl, album) || album.kind != LinkKind::Album) {
			return CStringW();
		}
		CStringW file = audioUrl;
		const int query = file.FindOneOf(L"?#");
		if (query != -1) {
			file.Truncate(query);
		}
		file = file.Mid(file.ReverseFind(L'/') + 1);
		if (file.IsEmpty()) {
			return CStringW();
		}
		file.Replace(L"%", L"%25");
		const CStringW page = album.albumUrl + L"/" + file;
		return TrackKeyOf(page).IsEmpty() ? CStringW() : page;
	}

	int FindTrack(const std::vector<KHInsider::Track>& tracks, const CStringW& trackKey)
	{
		if (trackKey.IsEmpty()) {
			return -1;
		}
		for (size_t i = 0; i < tracks.size(); i++) {
			if (TrackKeyOf(tracks[i].pageUrl) == trackKey) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	CStringW TrackShareText(const CStringW& trackName, const CStringW& albumTitle, const CStringW& trackPageUrl)
	{
		Link link;
		if (!ParseLink(trackPageUrl, link) || link.kind != LinkKind::Track) {
			return CStringW();
		}
		// the page's own file name (as encoded) under the rebuilt https album address
		CStringW path = trackPageUrl;
		const int query = path.FindOneOf(L"?#");
		if (query != -1) {
			path.Truncate(query);
		}
		path.TrimRight(L'/');
		const CStringW url = link.albumUrl + L"/" + path.Mid(path.ReverseFind(L'/') + 1);
		CStringW line = OneLine(trackName);
		const CStringW album = OneLine(albumTitle);
		if (!album.IsEmpty()) {
			line += (line.IsEmpty() ? L"" : L" \x2014 ") + album;
		}
		return line.IsEmpty() ? url : line + L"\r\n" + url;
	}

	CStringW AlbumShareText(const CStringW& albumTitle, const CStringW& albumUrl)
	{
		Link link;
		if (!ParseLink(albumUrl, link) || link.kind != LinkKind::Album) {
			return CStringW();
		}
		const CStringW line = OneLine(albumTitle);
		return line.IsEmpty() ? link.albumUrl : line + L"\r\n" + link.albumUrl;
	}

	namespace
	{
		bool OpenClipboardRetry(HWND owner)
		{
			// another program may hold it for a moment
			for (int i = 0; i < 8; i++) {
				if (::OpenClipboard(owner)) {
					return true;
				}
				Sleep(15);
			}
			return false;
		}
	}

	bool CopyToClipboard(HWND owner, const CStringW& text)
	{
		if (text.IsEmpty() || !OpenClipboardRetry(owner)) {
			return false;
		}
		bool ok = false;
		const SIZE_T bytes = (static_cast<SIZE_T>(text.GetLength()) + 1) * sizeof(wchar_t);
		if (::EmptyClipboard()) {
			if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
				if (void* dst = ::GlobalLock(mem)) {
					memcpy(dst, text.GetString(), bytes);
					::GlobalUnlock(mem);
					ok = ::SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
				}
				if (!ok) {
					::GlobalFree(mem);
				}
			}
		}
		::CloseClipboard();
		return ok;
	}

	CStringW ReadClipboard(HWND owner, int maxChars)
	{
		CStringW text;
		if (!::IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboardRetry(owner)) {
			return text;
		}
		if (HANDLE mem = ::GetClipboardData(CF_UNICODETEXT)) {
			const SIZE_T size = ::GlobalSize(mem) / sizeof(wchar_t);
			if (const auto* src = static_cast<const wchar_t*>(::GlobalLock(mem))) {
				const size_t limit = std::min<size_t>(size, static_cast<size_t>(std::max(0, maxChars)));
				text.SetString(src, static_cast<int>(wcsnlen(src, limit)));
				::GlobalUnlock(mem);
			}
		}
		::CloseClipboard();
		return text;
	}
}
