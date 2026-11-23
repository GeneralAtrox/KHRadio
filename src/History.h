/*
 * KH Radio - listening history (khradio_history.json).
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

// Persistent record of every album/track that has been listened to.
// Saves are atomic with one rolling "<file>.bak". A file that can't be read or
// parsed is renamed aside ("<file>.corrupt-<time>"), the backup is tried, and an
// empty list is never written over existing data.
class CKHRadioHistory
{
public:
	struct TrackEntry {
		CStringW name;
		int plays = 0;
	};

	struct AlbumEntry {
		CStringW url;
		CStringW title;
		std::vector<TrackEntry> tracks;
	};

	enum class LoadResult {
		Missing,             // no file yet: start empty
		Loaded,
		RecoveredFromBackup, // the file was bad; it was set aside and the .bak used
		Corrupt,             // the file was bad and no usable backup: start empty
	};

	LoadResult Load(const CStringW& path);
	bool Save() const;

	// Returns the album's index before this call (-1 if it is new); afterwards
	// the album is at index 0 (most recently listened first). Saves.
	int RecordTrack(const CStringW& albumUrl, const CStringW& albumTitle, const CStringW& trackName);

	const std::vector<AlbumEntry>& Albums() const { return m_albums; }
	bool IsReadOnly() const { return m_readOnly; }

	// The JSON format, exposed for the tests.
	static bool ParseJson(const char* text, std::vector<AlbumEntry>& albums);
	static std::string ToJson(const std::vector<AlbumEntry>& albums);

private:
	CStringW m_path;
	bool m_readOnly = false; // the bad file could not be set aside: never write this session
	std::vector<AlbumEntry> m_albums;
};
