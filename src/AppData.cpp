// KH Radio - data locations, settings.json, queue.json and first-run carry-over.
// GPL-3.0-or-later (see COPYING.txt).
#include "stdafx.h"
#include "AppData.h"
#include "FileUtil.h"
#include "JsonHelper.h"
#include "KHParse.h"
#include "KHWebSession.h"
#include "Log.h"
#include <shlobj.h>

namespace KHRadioData
{
	namespace
	{
		Settings g_settings;
		bool g_saveBlocked = false; // settings.json was unreadable and could not be set aside

		CStringW KnownFolder(REFKNOWNFOLDERID id)
		{
			PWSTR p = nullptr;
			CStringW result;
			if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) {
				result = p;
			}
			CoTaskMemFree(p);
			return result;
		}

		CStringW EnsureDir(const CStringW& dir)
		{
			SHCreateDirectoryExW(nullptr, dir, nullptr);
			return dir;
		}

		bool DirectoryIsEmpty(const CStringW& dir)
		{
			if (!PathIsDirectoryW(dir)) {
				return true;
			}
			return !!PathIsDirectoryEmptyW(dir);
		}

		// HKCU\Software\MPC-BE\Settings, opened read-only.
		constexpr wchar_t kForkSettingsKey[] = L"Software\\MPC-BE\\Settings";

		bool ReadForkBool(LPCWSTR name, bool& value)
		{
			DWORD v = 0, size = sizeof(v);
			if (RegGetValueW(HKEY_CURRENT_USER, kForkSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS) {
				if (v == 0 || v == 1) { // the fork's profile reader is just as strict
					value = (v == 1);
					return true;
				}
			}
			return false;
		}

		bool ReadForkString(LPCWSTR name, CStringW& value)
		{
			DWORD size = 0;
			if (RegGetValueW(HKEY_CURRENT_USER, kForkSettingsKey, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
				return false;
			}
			CStringW buf;
			const LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, kForkSettingsKey, name, RRF_RT_REG_SZ, nullptr,
				buf.GetBuffer(static_cast<int>(size / sizeof(wchar_t)) + 1), &size);
			buf.ReleaseBuffer();
			if (st != ERROR_SUCCESS) {
				return false;
			}
			value = buf;
			return true;
		}

		void ImportForkSettings(Settings& s)
		{
			int found = 0;
			found += ReadForkBool(L"KHRadioAvoidPlayed", s.avoidPlayed);
			found += ReadForkBool(L"KHRadioFilterSpoken", s.filterSpoken);
			found += ReadForkBool(L"KHRadioExclusive", s.exclusive);
			found += ReadForkString(L"KHRadioTypes", s.types);
			found += ReadForkString(L"KHRadioYears", s.years);
			found += ReadForkString(L"KHRadioPlatforms", s.platforms);
			KHLog::Format(L"carry-over: imported %d KHRadio setting(s) from HKCU\\%s", found, kForkSettingsKey);
		}

		void CarryOverHistory()
		{
			const CStringW dst = HistoryPath();
			if (KHFile::Exists(dst)) {
				return;
			}
			const CStringW src = KnownFolder(FOLDERID_RoamingAppData) + L"\\MPC-BE\\khradio_history.json";
			if (!KHFile::Exists(src)) {
				return;
			}
			const BOOL ok = CopyFileW(src, dst, TRUE);
			KHLog::Format(ok ? L"carry-over: copied history from %s" : L"carry-over: could not copy history from %s", src.GetString());
		}

		// Folders WebView2 rebuilds by itself; skipping them keeps the copy small.
		bool IsCacheFolder(LPCWSTR name)
		{
			static const LPCWSTR skip[] = {
				L"Cache", L"Code Cache", L"GPUCache", L"ShaderCache", L"GrShaderCache", L"GraphiteDawnCache",
				L"DawnCache", L"DawnGraphiteCache", L"DawnWebGPUCache", L"CacheStorage", L"ScriptCache", L"Crashpad",
				L"Service Worker",
			};
			for (const auto s : skip) {
				if (_wcsicmp(name, s) == 0) {
					return true;
				}
			}
			return false;
		}

		bool CopyTree(const CStringW& src, const CStringW& dst, int& files)
		{
			if (!CreateDirectoryW(dst, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
				return false;
			}
			WIN32_FIND_DATAW fd;
			HANDLE h = FindFirstFileW(src + L"\\*", &fd);
			if (h == INVALID_HANDLE_VALUE) {
				return true;
			}
			bool ok = true;
			do {
				if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
					continue;
				}
				const CStringW from = src + L"\\" + fd.cFileName;
				const CStringW to = dst + L"\\" + fd.cFileName;
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
					continue;
				}
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					if (!IsCacheFolder(fd.cFileName)) {
						ok = CopyTree(from, to, files);
					}
				} else {
					ok = !!CopyFileW(from, to, TRUE);
					files += ok;
				}
			} while (ok && FindNextFileW(h, &fd));
			FindClose(h);
			return ok;
		}

		void DeleteTree(const CStringW& dir)
		{
			// only ever called on the new, just-created profile copy
			CStringW from = dir;
			from.AppendChar(L'\0'); // SHFileOperation wants a double-NUL list
			SHFILEOPSTRUCTW op = {};
			op.wFunc = FO_DELETE;
			op.pFrom = from.GetString();
			op.fFlags = FOF_NO_UI;
			SHFileOperationW(&op);
		}

		void CarryOverWebViewProfile()
		{
			const CStringW dst = WebViewProfileDir();
			if (!DirectoryIsEmpty(dst)) {
				return;
			}
			const CStringW src = KnownFolder(FOLDERID_LocalAppData) + L"\\MPC-BE\\KHInsider.WebView2";
			if (!PathIsDirectoryW(src) || PathIsDirectoryEmptyW(src)) {
				return;
			}
			// A running browser holds EBWebView\lockfile open; copying a live profile
			// would give a torn cookie database, so skip quietly in that case.
			const CStringW lock = src + L"\\EBWebView\\lockfile";
			if (KHFile::Exists(lock)) {
				HANDLE h = CreateFileW(lock, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
				if (h == INVALID_HANDLE_VALUE) {
					KHLog::Line(L"carry-over: the fork's WebView2 profile is in use - not copied");
					return;
				}
				CloseHandle(h);
			}
			EnsureDir(dst);
			int files = 0;
			if (CopyTree(src, dst, files)) {
				KHLog::Format(L"carry-over: copied WebView2 profile (%d files) from %s", files, src.GetString());
			} else {
				DeleteTree(dst); // our own partial copy only; the source is untouched
				KHLog::Line(L"carry-over: WebView2 profile copy failed (files locked?) - skipped");
			}
		}

		// The cover file name for a saved coverPath, re-rooted in today's CoverDir().
		CStringW CoverInCache(const CStringW& saved)
		{
			if (saved.IsEmpty()) {
				return CStringW();
			}
			const CStringW name = PathFindFileNameW(saved);
			if (name.Left(6).CompareNoCase(L"cover_") != 0 || name.FindOneOf(L"\\/:") != -1) {
				return CStringW();
			}
			return CoverDir() + name;
		}
	}

	CStringW AppDataDir()
	{
		return EnsureDir(KnownFolder(FOLDERID_RoamingAppData) + L"\\KHRadio\\");
	}

	CStringW LocalDataDir()
	{
		return EnsureDir(KnownFolder(FOLDERID_LocalAppData) + L"\\KHRadio\\");
	}

	CStringW CoverDir()
	{
		return EnsureDir(LocalDataDir() + L"covers\\");
	}

	CStringW WebViewProfileDir()
	{
		return LocalDataDir() + L"WebView2";
	}

	CStringW CacheDir()
	{
		return EnsureDir(LocalDataDir() + L"cache\\");
	}

	CStringW DebugDir()
	{
		return LocalDataDir() + L"debug\\";
	}

	CStringW HistoryPath()
	{
		return AppDataDir() + L"khradio_history.json";
	}

	CStringW LogPath()
	{
		return AppDataDir() + L"khradio_debug.log";
	}

	Settings& Get()
	{
		return g_settings;
	}

	LoadResult Load()
	{
		Settings& s = g_settings;
		s = Settings();
		g_saveBlocked = false;
		const CStringW path = AppDataDir() + L"settings.json";
		std::vector<char> data;
		const auto read = KHFile::ReadAll(path, data);
		if (read == KHFile::ReadResult::Missing) {
			return LoadResult::Missing;
		}
		rapidjson::Document json;
		if (read == KHFile::ReadResult::Ok) {
			json.Parse(data.data());
		}
		if (read != KHFile::ReadResult::Ok || json.HasParseError() || !json.IsObject()) {
			// Keep the user's file: set it aside, or if even that fails, never write over it.
			const CStringW aside = KHFile::SetAside(path, L"unreadable");
			if (aside.IsEmpty()) {
				g_saveBlocked = true;
				KHLog::Format(L"settings.json could not be read and could not be set aside - using defaults, not saving this session");
			} else {
				KHLog::Format(L"settings.json could not be read - kept as %s, using defaults", aside.GetString());
			}
			s.carryOverDone = true; // not a first run
			return LoadResult::Unreadable;
		}
		getJsonValue(json, "types", s.types);
		getJsonValue(json, "years", s.years);
		getJsonValue(json, "platforms", s.platforms);
		getJsonValue(json, "avoidPlayed", s.avoidPlayed);
		getJsonValue(json, "filterSpoken", s.filterSpoken);
		getJsonValue(json, "exclusive", s.exclusive);
		getJsonValue(json, "volume", s.volume);
		getJsonValue(json, "saveDebugPages", s.saveDebugPages);
		getJsonValue(json, "preferAudioFormat", s.preferAudioFormat);
		getJsonValue(json, "carryOverDone", s.carryOverDone);
		s.volume = std::clamp(s.volume, 0.0, 1.0);
		if (const auto* w = GetJsonObject(json, "window")) {
			int l = 0, t = 0, r = 0, b = 0;
			if (getJsonValue(*w, "left", l) && getJsonValue(*w, "top", t) && getJsonValue(*w, "right", r) && getJsonValue(*w, "bottom", b)
					&& r > l && b > t) {
				s.normalRect = { l, t, r, b };
				s.hasPlacement = true;
			}
			getJsonValue(*w, "maximized", s.maximized);
		}
		return LoadResult::Loaded;
	}

	bool Save()
	{
		if (g_saveBlocked) {
			return false;
		}
		const Settings& s = g_settings;
		rapidjson::StringBuffer sb;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
		w.StartObject();
		WriteJsonString(w, "types", s.types);
		WriteJsonString(w, "years", s.years);
		WriteJsonString(w, "platforms", s.platforms);
		w.Key("avoidPlayed"); w.Bool(s.avoidPlayed);
		w.Key("filterSpoken"); w.Bool(s.filterSpoken);
		w.Key("exclusive"); w.Bool(s.exclusive);
		w.Key("volume"); w.Double(s.volume);
		w.Key("saveDebugPages"); w.Bool(s.saveDebugPages);
		if (!s.preferAudioFormat.IsEmpty()) {
			WriteJsonString(w, "preferAudioFormat", s.preferAudioFormat);
		}

		w.Key("carryOverDone"); w.Bool(s.carryOverDone);
		if (s.hasPlacement) {
			w.Key("window");
			w.StartObject();
			w.Key("left"); w.Int(s.normalRect.left);
			w.Key("top"); w.Int(s.normalRect.top);
			w.Key("right"); w.Int(s.normalRect.right);
			w.Key("bottom"); w.Int(s.normalRect.bottom);
			w.Key("maximized"); w.Bool(s.maximized);
			w.EndObject();
		}
		w.EndObject();
		if (!KHFile::WriteAtomic(AppDataDir() + L"settings.json", sb.GetString(), sb.GetSize())) {
			KHLog::Format(L"settings.json could not be saved (error %lu)", GetLastError());
			return false;
		}
		return true;
	}

	void RunCarryOver(LoadResult settingsResult)
	{
		Settings& s = g_settings;
		if (s.carryOverDone) {
			return;
		}
		if (settingsResult == LoadResult::Missing) {
			ImportForkSettings(s);
			CarryOverHistory();
			CarryOverWebViewProfile();
		}
		// An upgrade from 1.0 (settings already there) only records the marker.
		s.carryOverDone = true;
		Save();
	}

	void MigrateCoverCache()
	{
		const CStringW old = AppDataDir() + L"covers\\";
		if (!PathIsDirectoryW(old)) {
			return;
		}
		const CStringW dst = CoverDir();
		int moved = 0;
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW(old + L"*", &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					continue;
				}
				const CStringW from = old + fd.cFileName;
				if (_wcsnicmp(fd.cFileName, L"cover_", 6) == 0 && MoveFileExW(from, dst + fd.cFileName, MOVEFILE_COPY_ALLOWED)) {
					moved++;
				} else {
					DeleteFileW(from); // a duplicate or a leftover part file: only a cache
				}
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
		const BOOL removed = RemoveDirectoryW(old);
		KHLog::Format(L"covers: moved %d file(s) to %s%s", moved, dst.GetString(), removed ? L"" : L" (old folder not empty, left)");
	}

	SavedQueue LoadQueue()
	{
		SavedQueue q;
		const CStringW path = AppDataDir() + L"queue.json";
		std::vector<char> data;
		const auto read = KHFile::ReadAll(path, data);
		if (read == KHFile::ReadResult::Missing) {
			return q;
		}
		rapidjson::Document json;
		if (read == KHFile::ReadResult::Ok) {
			json.Parse(data.data());
		}
		if (read != KHFile::ReadResult::Ok || json.HasParseError() || !json.IsObject()) {
			const CStringW aside = KHFile::SetAside(path, L"unreadable");
			KHLog::Format(L"queue.json could not be read - starting with an empty queue%s%s",
				aside.IsEmpty() ? L"" : L", kept as ", aside.GetString());
			return q;
		}
		int savedIndex = -1;
		getJsonValue(json, "index", savedIndex);
		getJsonValue(json, "position", q.position);
		int dropped = 0;
		if (const auto* items = GetJsonArray(json, "items")) {
			int i = 0;
			for (const auto& v : items->GetArray()) {
				const int at = i++;
				if (!v.IsObject()) {
					continue;
				}
				QueueItem item;
				getJsonValue(v, "audioUrl", item.audioUrl);
				getJsonValue(v, "pageUrl", item.pageUrl);
				getJsonValue(v, "label", item.label);
				getJsonValue(v, "name", item.ref.name);
				getJsonValue(v, "albumUrl", item.ref.albumUrl);
				getJsonValue(v, "albumTitle", item.ref.albumTitle);
				getJsonValue(v, "coverPath", item.ref.coverPath);
				getJsonValue(v, "platforms", item.ref.platforms);
				// The file is outside the app's control: only KHInsider addresses are played,
				// and covers only come from the app's own cache folder.
				if (!item.audioUrl.IsEmpty() && !KHInsider::IsAllowedMediaUrl(item.audioUrl)) {
					item.audioUrl.Empty();
				}
				if (!item.pageUrl.IsEmpty() && !KHWebSession::IsSiteUrl(item.pageUrl.GetString())) {
					item.pageUrl.Empty();
				}
				if (!item.ref.albumUrl.IsEmpty() && !KHWebSession::IsSiteUrl(item.ref.albumUrl.GetString())) {
					item.ref.albumUrl.Empty();
				}
				item.ref.coverPath = CoverInCache(item.ref.coverPath);
				if (item.audioUrl.IsEmpty() && item.pageUrl.IsEmpty()) {
					dropped++;
					if (at < savedIndex) {
						savedIndex--;
					}
					continue;
				}
				item.id = NewQueueItemId();
				q.items.emplace_back(item);
			}
		}
		if (dropped) {
			KHLog::Format(L"queue.json: dropped %d item(s) that were not KHInsider addresses", dropped);
		}
		q.index = savedIndex;
		if (q.index < 0 || q.index >= static_cast<int>(q.items.size())) {
			q.index = q.items.empty() ? -1 : 0;
			q.position = 0.0;
		}
		return q;
	}

	bool SaveQueue(const std::vector<QueueItem>& items, int index, double position)
	{
		// keep a short tail of already-played tracks, not the whole session
		const size_t first = static_cast<size_t>(std::max(0, index - 50));
		rapidjson::StringBuffer sb;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
		w.StartObject();
		w.Key("index"); w.Int(index < 0 ? -1 : index - static_cast<int>(first));
		w.Key("position"); w.Double(position);
		w.Key("items");
		w.StartArray();
		for (size_t i = first; i < items.size(); i++) {
			const auto& item = items[i];
			w.StartObject();
			WriteJsonString(w, "audioUrl", item.audioUrl);
			WriteJsonString(w, "pageUrl", item.pageUrl);
			WriteJsonString(w, "label", item.label);
			WriteJsonString(w, "name", item.ref.name);
			WriteJsonString(w, "albumUrl", item.ref.albumUrl);
			WriteJsonString(w, "albumTitle", item.ref.albumTitle);
			WriteJsonString(w, "coverPath", item.ref.coverPath);
			WriteJsonString(w, "platforms", item.ref.platforms);
			w.EndObject();
		}
		w.EndArray();
		w.EndObject();
		if (!KHFile::WriteAtomic(AppDataDir() + L"queue.json", sb.GetString(), sb.GetSize())) {
			KHLog::Format(L"queue.json could not be saved (error %lu)", GetLastError());
			return false;
		}
		return true;
	}
}
