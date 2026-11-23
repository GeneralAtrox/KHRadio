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

#include "stdafx.h"
#include "History.h"
#include "FileUtil.h"
#include "JsonHelper.h"
#include "Log.h"

bool CKHRadioHistory::ParseJson(const char* text, std::vector<AlbumEntry>& albums)
{
	albums.clear();
	rapidjson::Document json;
	json.Parse(text);
	if (json.HasParseError() || !json.IsObject()) {
		return false;
	}
	const auto it = json.FindMember("albums");
	if (it == json.MemberEnd() || !it->value.IsArray()) {
		return false;
	}
	for (const auto& a : it->value.GetArray()) {
		if (!a.IsObject()) {
			continue;
		}
		AlbumEntry entry;
		getJsonValue(a, "url", entry.url);
		getJsonValue(a, "title", entry.title);
		if (entry.url.IsEmpty()) {
			continue;
		}
		if (const auto* tracks = GetJsonArray(a, "tracks")) {
			for (const auto& t : tracks->GetArray()) {
				if (!t.IsObject()) {
					continue;
				}
				TrackEntry te;
				getJsonValue(t, "name", te.name);
				getJsonValue(t, "plays", te.plays);
				if (!te.name.IsEmpty()) {
					entry.tracks.emplace_back(te);
				}
			}
		}
		albums.emplace_back(entry);
	}
	return true;
}

std::string CKHRadioHistory::ToJson(const std::vector<AlbumEntry>& albums)
{
	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("albums");
	w.StartArray();
	for (const auto& album : albums) {
		w.StartObject();
		WriteJsonString(w, "url", album.url);
		WriteJsonString(w, "title", album.title);
		w.Key("tracks");
		w.StartArray();
		for (const auto& track : album.tracks) {
			w.StartObject();
			WriteJsonString(w, "name", track.name);
			w.Key("plays");
			w.Int(track.plays);
			w.EndObject();
		}
		w.EndArray();
		w.EndObject();
	}
	w.EndArray();
	w.EndObject();
	return std::string(sb.GetString(), sb.GetSize());
}

CKHRadioHistory::LoadResult CKHRadioHistory::Load(const CStringW& path)
{
	m_path = path;
	m_readOnly = false;
	m_albums.clear();

	std::vector<char> data;
	const auto read = KHFile::ReadAll(path, data);
	if (read == KHFile::ReadResult::Missing) {
		return LoadResult::Missing;
	}
	if (read == KHFile::ReadResult::Ok && ParseJson(data.data(), m_albums)) {
		return LoadResult::Loaded;
	}

	// Unreadable or cut short: keep the file for the user, never write over it.
	m_albums.clear();
	const CStringW aside = KHFile::SetAside(path, L"corrupt");
	if (aside.IsEmpty()) {
		m_readOnly = true;
		KHLog::Format(L"history: %s could not be %s and could not be set aside - history will not be saved this session",
			path.GetString(), read == KHFile::ReadResult::Ok ? L"parsed" : L"read");
		return LoadResult::Corrupt;
	}
	KHLog::Format(L"history: %s could not be %s - kept as %s", path.GetString(),
		read == KHFile::ReadResult::Ok ? L"parsed" : L"read", aside.GetString());

	std::vector<char> backup;
	if (KHFile::ReadAll(path + L".bak", backup) == KHFile::ReadResult::Ok && ParseJson(backup.data(), m_albums)) {
		KHLog::Format(L"history: restored %u album(s) from the backup", static_cast<unsigned>(m_albums.size()));
		return LoadResult::RecoveredFromBackup;
	}
	m_albums.clear();
	return LoadResult::Corrupt;
}

bool CKHRadioHistory::Save() const
{
	if (m_path.IsEmpty() || m_readOnly) {
		return false;
	}
	if (m_albums.empty()) {
		// History only ever grows; an empty list here means something went wrong,
		// so never let it replace what is on disk.
		return false;
	}
	const std::string json = ToJson(m_albums);
	if (!KHFile::WriteAtomic(m_path, json.data(), json.size(), true)) {
		KHLog::Format(L"history: could not save %s (error %lu)", m_path.GetString(), GetLastError());
		return false;
	}
	return true;
}

int CKHRadioHistory::RecordTrack(const CStringW& albumUrl, const CStringW& albumTitle, const CStringW& trackName)
{
	if (albumUrl.IsEmpty()) {
		return -1;
	}

	int previous = -1;
	for (size_t i = 0; i < m_albums.size(); i++) {
		if (m_albums[i].url.CompareNoCase(albumUrl) == 0) {
			previous = static_cast<int>(i);
			break;
		}
	}

	if (previous == -1) {
		AlbumEntry entry;
		entry.url = albumUrl;
		entry.title = albumTitle;
		m_albums.insert(m_albums.begin(), entry);
	} else if (previous > 0) {
		// most recently listened first
		AlbumEntry entry = m_albums[static_cast<size_t>(previous)];
		m_albums.erase(m_albums.begin() + previous);
		m_albums.insert(m_albums.begin(), entry);
	}

	AlbumEntry& album = m_albums.front();
	if (!albumTitle.IsEmpty()) {
		album.title = albumTitle;
	}

	if (!trackName.IsEmpty()) {
		bool found = false;
		for (auto& track : album.tracks) {
			if (track.name.CompareNoCase(trackName) == 0) {
				track.plays++;
				found = true;
				break;
			}
		}
		if (!found) {
			TrackEntry te;
			te.name = trackName;
			te.plays = 1;
			album.tracks.emplace_back(te);
		}
	}

	Save();
	return previous;
}
