// KH Radio - debug log (khradio_debug.log), safe to call from any thread, also during exit.
// GPL-3.0-or-later (see COPYING.txt).
#include "stdafx.h"
#include "Log.h"
#include "TextUtil.h"

namespace KHLog
{
	namespace
	{
		// Plain storage with no destructors: a worker thread that is still logging
		// while the process exits must not find a destroyed lock or string.
		SRWLOCK g_lock = SRWLOCK_INIT;
		wchar_t g_path[MAX_PATH * 2] = {};
		constexpr LONGLONG kMaxLogBytes = 4ll * 1024 * 1024;
	}

	void SetPath(const CStringW& path)
	{
		AcquireSRWLockExclusive(&g_lock);
		wcsncpy_s(g_path, path.GetString(), _TRUNCATE);
		ReleaseSRWLockExclusive(&g_lock);
	}

	void Line(const CStringW& text)
	{
		SYSTEMTIME st;
		GetLocalTime(&st);
		CStringW line;
		line.Format(L"[%02u:%02u:%02u.%03u] %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, text.GetString());
		const CStringA utf8 = WStrToUTF8(line);

		AcquireSRWLockExclusive(&g_lock);
		if (g_path[0]) {
			HANDLE h = CreateFileW(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h != INVALID_HANDLE_VALUE) {
				LARGE_INTEGER size = {};
				if (GetFileSizeEx(h, &size) && size.QuadPart > kMaxLogBytes) {
					// rotate by size during the session, not only at startup
					CloseHandle(h);
					wchar_t old[MAX_PATH * 2 + 8];
					swprintf_s(old, L"%s.old", g_path);
					MoveFileExW(g_path, old, MOVEFILE_REPLACE_EXISTING);
					h = CreateFileW(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
						nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				}
				if (h != INVALID_HANDLE_VALUE) {
					DWORD written = 0;
					WriteFile(h, utf8.GetString(), static_cast<DWORD>(utf8.GetLength()), &written, nullptr);
					CloseHandle(h);
				}
			}
		}
		ReleaseSRWLockExclusive(&g_lock);
	}

	void Format(LPCWSTR fmt, ...)
	{
		CStringW msg;
		va_list args;
		va_start(args, fmt);
		msg.FormatV(fmt, args);
		va_end(args);
		Line(msg);
	}
}
