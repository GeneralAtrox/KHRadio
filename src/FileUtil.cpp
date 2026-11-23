// KH Radio - small file helpers: whole-file reads, atomic writes, setting a bad file aside.
// GPL-3.0-or-later (see COPYING.txt).
#include "stdafx.h"
#include "FileUtil.h"

namespace KHFile
{
	bool Exists(const CStringW& path)
	{
		return !path.IsEmpty() && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
	}

	ReadResult ReadAll(const CStringW& path, std::vector<char>& data, size_t maxBytes)
	{
		data.clear();
		HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			const DWORD err = GetLastError();
			return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? ReadResult::Missing : ReadResult::Error;
		}
		LARGE_INTEGER size = {};
		bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && static_cast<ULONGLONG>(size.QuadPart) <= maxBytes;
		if (ok) {
			const DWORD len = static_cast<DWORD>(size.QuadPart);
			data.resize(static_cast<size_t>(len) + 1);
			DWORD read = 0;
			ok = ReadFile(h, data.data(), len, &read, nullptr) && read == len;
			data[len] = '\0';
		}
		CloseHandle(h);
		if (!ok) {
			data.clear();
			return ReadResult::Error;
		}
		return ReadResult::Ok;
	}

	bool WriteAtomic(const CStringW& path, const void* bytes, size_t len, bool keepBackup)
	{
		if (path.IsEmpty() || len > MAXDWORD) {
			return false;
		}
		const CStringW tmp = path + L".tmp";
		HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			return false;
		}
		DWORD written = 0;
		bool ok = WriteFile(h, bytes, static_cast<DWORD>(len), &written, nullptr) && written == len;
		// make sure the data is on disk before the rename makes it the real file
		ok = ok && FlushFileBuffers(h);
		CloseHandle(h);
		if (!ok) {
			DeleteFileW(tmp);
			return false;
		}
		if (keepBackup && Exists(path)) {
			// the old file is intact until the rename below, so a failed copy only
			// costs the backup, never the data
			CopyFileW(path, path + L".bak", FALSE);
		}
		if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			DeleteFileW(tmp);
			return false;
		}
		return true;
	}

	CStringW SetAside(const CStringW& path, LPCWSTR tag)
	{
		SYSTEMTIME st;
		GetLocalTime(&st);
		CStringW aside;
		aside.Format(L"%s.%s-%04u%02u%02u-%02u%02u%02u", path.GetString(), tag,
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		// never replace an earlier set-aside copy (two in the same second get -2, -3, ...)
		for (int n = 1; n <= 20; n++) {
			CStringW name = aside;
			if (n > 1) {
				name.AppendFormat(L"-%d", n);
			}
			if (MoveFileExW(path, name, MOVEFILE_WRITE_THROUGH)) {
				return name;
			}
			if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) {
				break;
			}
		}
		return CStringW();
	}
}
