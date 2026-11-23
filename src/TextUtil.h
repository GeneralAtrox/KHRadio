// KH Radio - UTF-8 helpers copied from MPC-BE's DSUtil/text.cpp. GPL-3.0-or-later.
#pragma once
#include <atlstr.h>

inline CStringA WStrToUTF8(LPCWSTR lpWideCharStr)
{
	CStringA str;
	int len = WideCharToMultiByte(CP_UTF8, 0, lpWideCharStr, -1, nullptr, 0, nullptr, nullptr) - 1;
	if (len > 0) {
		str.ReleaseBuffer(WideCharToMultiByte(CP_UTF8, 0, lpWideCharStr, -1, str.GetBuffer(len), len + 1, nullptr, nullptr) - 1);
	}
	return str;
}

inline CStringW UTF8ToWStr(LPCSTR lpUTF8Str)
{
	CStringW str;
	int len = MultiByteToWideChar(CP_UTF8, 0, lpUTF8Str, -1, nullptr, 0) - 1;
	if (len > 0) {
		str.ReleaseBuffer(MultiByteToWideChar(CP_UTF8, 0, lpUTF8Str, -1, str.GetBuffer(len), len + 1) - 1);
	}
	return str;
}
