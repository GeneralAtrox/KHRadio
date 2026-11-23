// KH Radio - sharing a song: the text put on the clipboard, and finding a
// KHInsider album or track link in pasted text. Pure string work (plus two
// clipboard helpers), so the tests can drive it. GPL-3.0-or-later (see COPYING.txt).
#pragma once

#include <atlstr.h>
#include <vector>
#include "KHParse.h"

namespace KHShare
{
	enum class LinkKind { None, Album, Track };

	struct Link {
		LinkKind kind = LinkKind::None;
		// https://downloads.khinsider.com/game-soundtracks/album/<slug>, rebuilt from
		// the parts that passed validation: the only address ever fetched.
		CStringW albumUrl;
		// Track links only: the file name, percent-decoded (repeatedly) and lower
		// case. Used only to find the track on the album page, never fetched.
		CStringW trackKey;
	};

	// One address (no surrounding text). Accepts https://, http:// (upgraded) or no
	// scheme, and an optional "www." before downloads.khinsider.com. Refuses any
	// other host, user info, ports, other schemes and other paths.
	bool ParseLink(const CStringW& url, Link& link);

	// The first acceptable link anywhere in pasted text: tolerates surrounding
	// words, <angle brackets>, quotes, trailing punctuation, markdown links,
	// query strings and fragments. Only the first 64 K characters are looked at.
	bool FindLink(const CStringW& text, Link& link);

	// Decoded, lower-case file name of a track page address ("" if it is not one).
	CStringW TrackKeyOf(const CStringW& trackPageUrl);

	// The track page to share for a queue item. Items restored from a queue saved
	// by an older version have no page address; for those it is rebuilt from the
	// album page and the audio file name (which the site encodes once more).
	// "" if neither route gives a KHInsider track page.
	CStringW TrackPageFor(const CStringW& trackPageUrl, const CStringW& albumUrl, const CStringW& audioUrl);

	// Index in 'tracks' of the track whose page has this key, or -1.
	int FindTrack(const std::vector<KHInsider::Track>& tracks, const CStringW& trackKey);

	// "<track> — <album>\r\n<track page URL>". Empty if the address is not a
	// KHInsider track page (so nothing else can ever be shared).
	CStringW TrackShareText(const CStringW& trackName, const CStringW& albumTitle, const CStringW& trackPageUrl);
	// "<album>\r\n<album page URL>", empty if the address is not a KHInsider album.
	CStringW AlbumShareText(const CStringW& albumTitle, const CStringW& albumUrl);

	bool CopyToClipboard(HWND owner, const CStringW& text);
	// Unicode text on the clipboard ("" if none), at most maxChars characters.
	CStringW ReadClipboard(HWND owner, int maxChars = 65536);
}
