/*
 * KH Radio - pure parsing for downloads.khinsider.com pages (no network, no UI),
 * so the tests can drive it with small hand-written pages.
 *
 * Copyright (C) 2026 KH Radio authors.
 * Derived from the KH Radio panel of the MPC-BE fork (MPC-BE, GPLv3).
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this
 * program (COPYING.txt). If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <atlstr.h>
#include <string>
#include <vector>

namespace KHInsider
{
	struct Track {
		CStringW name;            // display name
		CStringW pageUrl;         // track page on downloads.khinsider.com
		int      number = 0;      // 1-based position in the album's track list
		int      durationSec = 0; // length from the album page (0 if unknown)
	};

	struct Album {
		CStringW url;       // album page URL
		CStringW title;
		// Cover candidates in the order to try: each image block's full-size scan
		// first, then the small thumbnails as fallbacks.
		std::vector<CStringW> coverUrls;
		CStringW platforms; // comma-separated console list ("Saturn, Arcade"), if any
		std::vector<Track> tracks;
	};

	// Result of analysing a track's audio for the "skip non-music" filter.
	enum class AudioVerdict {
		Music,   // keep it
		Spoken,  // dialogue/drama/narration
		Quiet,   // (near-)silent / empty
	};

	// Why a fetch failed, so the UI can show a useful message and we can tell
	// "site is down / blocking us" apart from "site changed its HTML".
	enum class FetchStatus {
		Success,
		NetworkError,  // no usable response (offline, timeout, DNS)
		Blocked,       // Cloudflare / bot protection or rate limit (403, 429, challenge page)
		LayoutChanged, // page fetched but it no longer has the structure we parse
		LoginRequired,
		BrowserUnavailable,
	};

	CStringW DecodeHtmlEntities(CStringW str);

	// Album page -> title, track pages (with durations), cover candidates and
	// platforms. Returns false when no track could be found. 'usedFallback' is set
	// when the songlist table marker was missing and the whole page was scanned.
	bool ParseAlbumPage(const std::string& html, const CStringW& pageUrl, Album& album, bool* usedFallback = nullptr);

	// Track page -> direct audio URL (the <audio> source, else an mp3 link, else
	// flac/m4a/ogg). Empty if none is found. A non-empty preferExt ("ogg", "m4a")
	// takes a download link in that format first, if the page offers one.
	CStringW ExtractTrackAudioUrl(const std::string& html, const CStringW& preferExt = CStringW());

	// Audio format by the address's file extension (query string and case ignored).
	enum class AudioFormat { Other, Mp3, Flac, M4a, Ogg };
	AudioFormat AudioFormatOf(const CStringW& url);

	// POST body for the random-album-advanced form. Each list holds the numeric
	// option values of the site's form ("type[]", "year[]", "category[]").
	CStringA BuildRandomAlbumForm(const std::vector<int>& types, const std::vector<int>& years, const std::vector<int>& platforms);

	// True if a title (a track name or a whole album title) contains drama/voice
	// keywords - lets the radio skip drama-CD / voice albums by title.
	bool IsSpokenTitle(const CStringW& title);

	// Size of a leading ID3v2 tag (header, body and footer), or 0 if there is none.
	size_t Id3v2TagSize(const unsigned char* data, size_t len);

	// Media (audio and cover) may only come over http(s) from KHInsider's own
	// hosts: *.khinsider.com and its CDN *.vgmtreasurechest.com.
	bool IsAllowedMediaUrl(const CStringW& url);

	// True if every console the album is tagged with is among the selected
	// consoles. An empty tag can't be verified, so it counts as exclusive
	// (fail-open) to avoid dead-ending the radio if the platform line won't parse.
	bool AlbumExclusiveTo(const CStringW& albumPlatforms, const std::vector<CStringW>& selected);
}
