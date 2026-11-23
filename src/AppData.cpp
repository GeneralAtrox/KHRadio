// KH Radio - data locations, settings.json, queue.json and first-run carry-over.
// GPL-3.0-or-later (see COPYING.txt).
#include "stdafx.h"
#include "AppData.h"
#include "JsonHelper.h"
#include "KHInsider.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/prettywriter.h"
#include <shlobj.h>

namespace KHRadioData
{
	namespace
	{
		Settings g_settings;

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

		bool ReadFileBytes(const CStringW& path, std::vector<char>& data)
		{
			HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				return false;
			}
			LARGE_INTEGER size = {};
			bool ok = GetFileSizeEx(h, &size) && size.QuadPart < 64 * 1024 * 1024;
			if (ok) {
				data.resize(static_cast<size_t>(size.QuadPart) + 1);
				DWORD read = 0;
				ok = ReadFile(h, data.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) && read == size.QuadPart;
				data[static_cast<size_t>(size.QuadPart)] = '\0';
			}
			CloseHandle(h);
			return ok;
		}

		// Write to a temp file, then replace, so a crash never leaves half a file.
		bool WriteFileAtomic(const CStringW& path, const char* bytes, size_t len)
		{
			const CStringW tmp = path + L".tmp";
			HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				return false;
			}
			DWORD written = 0;
			const bool ok = WriteFile(h, bytes, static_cast<DWORD>(len), &written, nullptr) && written == len;
			CloseHandle(h);
			if (!ok) {
				DeleteFileW(tmp);
				return false;
			}
			return !!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
		}

		void WriteString(rapidjson::PrettyWriter<rapidjson::StringBuffer>& w, const char* key, const CStringW& value)
		{
			const CStringA utf8 = WStrToUTF8(value);
			w.Key(key);
			w.String(utf8.GetString(), utf8.GetLength());
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
				if (v == 0 || v == 1) { // MPC-BE's CProfile::ReadBool is just as strict
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
				buf.GetBuffer(size / sizeof(wchar_t) + 1), &size);
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
			CStringW log;
			log.Format(L"carry-over: imported %d KHRadio setting(s) from HKCU\\%s", found, kForkSettingsKey);
			KHInsider::DebugLog(log);
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
	}

	CStringW AppDataDir()
	{
		CStringW dir = KnownFolder(FOLDERID_RoamingAppData) + L"\\KHRadio\\";
		SHCreateDirectoryExW(nullptr, dir, nullptr);
		return dir;
	}

	CStringW CoverDir()
	{
		CStringW dir = AppDataDir() + L"covers\\";
		SHCreateDirectoryExW(nullptr, dir, nullptr);
		return dir;
	}

	CStringW WebViewProfileDir()
	{
		return KnownFolder(FOLDERID_LocalAppData) + L"\\KHRadio\\WebView2";
	}

	Settings& Get()
	{
		return g_settings;
	}

	void Load()
	{
		Settings& s = g_settings;
		s = Settings();
		const CStringW path = AppDataDir() + L"settings.json";
		std::vector<char> data;
		if (!ReadFileBytes(path, data)) {
			if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
				ImportForkSettings(s);
			}
			Save();
			return;
		}
		rapidjson::Document json;
		json.Parse(data.data());
		if (json.HasParseError() || !json.IsObject()) {
			KHInsider::DebugLog(L"settings.json could not be parsed - using defaults");
			return;
		}
		getJsonValue(json, "types", s.types);
		getJsonValue(json, "years", s.years);
		getJsonValue(json, "platforms", s.platforms);
		getJsonValue(json, "avoidPlayed", s.avoidPlayed);
		getJsonValue(json, "filterSpoken", s.filterSpoken);
		getJsonValue(json, "exclusive", s.exclusive);
		getJsonValue(json, "volume", s.volume);
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
	}

	void Save()
	{
		const Settings& s = g_settings;
		rapidjson::StringBuffer sb;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
		w.StartObject();
		WriteString(w, "types", s.types);
		WriteString(w, "years", s.years);
		WriteString(w, "platforms", s.platforms);
		w.Key("avoidPlayed"); w.Bool(s.avoidPlayed);
		w.Key("filterSpoken"); w.Bool(s.filterSpoken);
		w.Key("exclusive"); w.Bool(s.exclusive);
		w.Key("volume"); w.Double(s.volume);
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
		WriteFileAtomic(AppDataDir() + L"settings.json", sb.GetString(), sb.GetSize());
	}

	void CarryOverHistory()
	{
		const CStringW dst = AppDataDir() + L"khradio_history.json";
		if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) {
			return;
		}
		const CStringW src = KnownFolder(FOLDERID_RoamingAppData) + L"\\MPC-BE\\khradio_history.json";
		if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) {
			return;
		}
		const BOOL ok = CopyFileW(src, dst, TRUE);
		KHInsider::DebugLog(ok ? CStringW(L"carry-over: copied history from ") + src
							   : CStringW(L"carry-over: could not copy history from ") + src);
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
		if (GetFileAttributesW(lock) != INVALID_FILE_ATTRIBUTES) {
			HANDLE h = CreateFileW(lock, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				KHInsider::DebugLog(L"carry-over: fork's WebView2 profile is in use - not copied");
				return;
			}
			CloseHandle(h);
		}
		SHCreateDirectoryExW(nullptr, dst, nullptr);
		int files = 0;
		if (CopyTree(src, dst, files)) {
			CStringW log;
			log.Format(L"carry-over: copied WebView2 profile (%d files) from %s", files, src.GetString());
			KHInsider::DebugLog(log);
		} else {
			DeleteTree(dst); // our own partial copy only; the source is untouched
			KHInsider::DebugLog(L"carry-over: WebView2 profile copy failed (files locked?) - skipped");
		}
	}

	SavedQueue LoadQueue()
	{
		SavedQueue q;
		std::vector<char> data;
		if (!ReadFileBytes(AppDataDir() + L"queue.json", data)) {
			return q;
		}
		rapidjson::Document json;
		json.Parse(data.data());
		if (json.HasParseError() || !json.IsObject()) {
			return q;
		}
		getJsonValue(json, "index", q.index);
		getJsonValue(json, "position", q.position);
		if (const auto* items = GetJsonArray(json, "items")) {
			for (const auto& v : items->GetArray()) {
				if (!v.IsObject()) {
					continue;
				}
				QueueItem item;
				getJsonValue(v, "audioUrl", item.audioUrl);
				getJsonValue(v, "label", item.label);
				getJsonValue(v, "name", item.ref.name);
				getJsonValue(v, "albumUrl", item.ref.albumUrl);
				getJsonValue(v, "albumTitle", item.ref.albumTitle);
				getJsonValue(v, "coverPath", item.ref.coverPath);
				getJsonValue(v, "platforms", item.ref.platforms);
				if (!item.audioUrl.IsEmpty()) {
					q.items.emplace_back(item);
				}
			}
		}
		if (q.index < 0 || q.index >= static_cast<int>(q.items.size())) {
			q.index = q.items.empty() ? -1 : 0;
			q.position = 0.0;
		}
		return q;
	}

	void SaveQueue(const std::vector<QueueItem>& items, int index, double position)
	{
		// keep a short tail of already-played tracks, not the whole session
		const int first = std::max(0, index - 50);
		rapidjson::StringBuffer sb;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
		w.StartObject();
		w.Key("index"); w.Int(index < 0 ? -1 : index - first);
		w.Key("position"); w.Double(position);
		w.Key("items");
		w.StartArray();
		for (size_t i = first; i < items.size(); i++) {
			const auto& item = items[i];
			w.StartObject();
			WriteString(w, "audioUrl", item.audioUrl);
			WriteString(w, "label", item.label);
			WriteString(w, "name", item.ref.name);
			WriteString(w, "albumUrl", item.ref.albumUrl);
			WriteString(w, "albumTitle", item.ref.albumTitle);
			WriteString(w, "coverPath", item.ref.coverPath);
			WriteString(w, "platforms", item.ref.platforms);
			w.EndObject();
		}
		w.EndArray();
		w.EndObject();
		WriteFileAtomic(AppDataDir() + L"queue.json", sb.GetString(), sb.GetSize());
	}
}
