/*
 * KH Radio - network side of the downloads.khinsider.com scraper: page fetches
 * through the signed-in browser session, cover downloads, the audio classifier.
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

#include "KHParse.h"
#include "AudioClassify.h"
#include <functional>
#include <vector>

namespace KHInsider
{
	// Opt-in diagnostics: when set, the last album page and track page fetched
	// are saved there (album.html / track.html). Empty turns it off.
	void SetDebugPageDir(const CStringW& dir);

	// Called once at exit: every download in progress or about to start gives
	// up quickly so the worker threads can be joined.
	void BeginShutdown();
	bool IsShuttingDown();

	// POST the random-album-advanced form; fills 'album' with title and track pages.
	bool FetchRandomAlbum(const CStringA& formBody, Album& album, FetchStatus* pStatus = nullptr);

	// GET an album page directly (used when replaying an album from history).
	bool FetchAlbum(const CStringW& albumUrl, Album& album, FetchStatus* pStatus = nullptr);

	// GET a track page and extract the direct audio URL (mp3 preferred). Returns
	// an empty string on failure; *pStatus says why (LayoutChanged = the page
	// loaded but had no usable audio link).
	CStringW ResolveTrackAudioUrl(const CStringW& trackPageUrl, FetchStatus* pStatus = nullptr);

	// Download a cover image to a local file; fails if it is bigger than maxBytes
	// or not on KHInsider's hosts.
	bool DownloadToFile(const CStringW& url, const CStringW& localPath, size_t maxBytes);

	// Download a whole track's audio to a file for the audio cache: bounded by
	// maxBytes, a 4-minute deadline, shutdown and 'cancelled'. Nothing is left
	// at localPath on failure.
	bool DownloadAudioToFile(const CStringW& url, const CStringW& localPath, size_t maxBytes, const std::function<bool()>& cancelled);

	// Testing aid (settings.json "preferAudioFormat"): take a track page's link
	// in this format ("ogg", "m4a", "flac", "mp3") first. Empty = normal order.
	// Call before any worker starts.
	void SetPreferredAudioFormat(const CStringW& ext);

	// The talking-track check of an mp3: two 256 KB windows (after the ID3 tag,
	// and the middle) fetched with ranged GETs and judged by KHAudio. Other
	// formats, and anything that fails, come back not judged (= music).
	KHAudio::Decision ClassifyTrackAudio(const CStringW& audioUrl);
	// The same decision on decoded audio (mono, -1..1): Ogg tracks, which the
	// cache decodes anyway. 'middle' may be empty.
	KHAudio::Decision ClassifyMonoPcm(const std::vector<float>& first, const std::vector<float>& middle, int hz);
}
