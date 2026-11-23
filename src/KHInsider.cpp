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

#include "stdafx.h"
#include "KHWebSession.h"
#include "TextUtil.h"
#include "FileUtil.h"
#include "Log.h"
#include "KHInsider.h"
#include "AudioClassify.h"

#include <shlobj.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <cmath>
#include <functional>
#include <mutex>
#include <string>


#define KHINSIDER_BASE L"https://downloads.khinsider.com"

// WinHTTP downloads (the audio prefix for the classifier, and covers when
// curl-impersonate is missing) present a browser user agent.
#define KH_USER_AGENT L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"

#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#endif
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP2
#define WINHTTP_PROTOCOL_FLAG_HTTP2 0x1
#endif

namespace KHInsider
{
	using urlData = std::vector<char>;

	namespace
	{
		std::atomic<bool> g_shuttingDown{false};

		// Manual-reset event set at shutdown; retry back-offs wait on it.
		HANDLE CancelEvent()
		{
			static const HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			return ev;
		}

		// Sleeps unless shutdown starts first; returns false if it did.
		bool WaitOrCancel(DWORD ms)
		{
			const HANDLE ev = CancelEvent();
			if (!ev) {
				Sleep(ms);
				return !g_shuttingDown;
			}
			return WaitForSingleObject(ev, ms) == WAIT_TIMEOUT;
		}

		SRWLOCK g_preferLock = SRWLOCK_INIT;
		wchar_t g_preferExt[8] = {};

		CStringW PreferredAudioFormat()
		{
			AcquireSRWLockShared(&g_preferLock);
			CStringW ext = g_preferExt;
			ReleaseSRWLockShared(&g_preferLock);
			return ext;
		}

		SRWLOCK g_debugLock = SRWLOCK_INIT;
		wchar_t g_debugDir[MAX_PATH * 2] = {};

		void SaveDebugPage(LPCWSTR name, const std::string& html)
		{
			wchar_t path[MAX_PATH * 2 + 32] = {};
			AcquireSRWLockShared(&g_debugLock);
			if (g_debugDir[0]) {
				swprintf_s(path, L"%s%s", g_debugDir, name);
			}
			ReleaseSRWLockShared(&g_debugLock);
			if (path[0]) {
				KHFile::WriteAtomic(path, html.data(), html.size());
			}
		}
	}

	void SetDebugPageDir(const CStringW& dir)
	{
		AcquireSRWLockExclusive(&g_debugLock);
		wcsncpy_s(g_debugDir, dir.GetString(), _TRUNCATE);
		ReleaseSRWLockExclusive(&g_debugLock);
		if (!dir.IsEmpty()) {
			SHCreateDirectoryExW(nullptr, dir, nullptr);
		}
	}

	void SetPreferredAudioFormat(const CStringW& ext)
	{
		CStringW e = ext;
		e.Trim(L" .");
		e.MakeLower();
		if (e != L"ogg" && e != L"m4a" && e != L"mp3" && e != L"flac") {
			e.Empty();
		}
		AcquireSRWLockExclusive(&g_preferLock);
		wcsncpy_s(g_preferExt, e.GetString(), _TRUNCATE);
		ReleaseSRWLockExclusive(&g_preferLock);
	}

	void BeginShutdown()
	{
		g_shuttingDown = true;
		if (const HANDLE ev = CancelEvent()) {
			SetEvent(ev);
		}
	}

	bool IsShuttingDown()
	{
		return g_shuttingDown;
	}

	// ---- libcurl-impersonate: Cloudflare-resilient transport (optional) -------
	//
	// downloads.khinsider.com and its cover CDN are fronted by Cloudflare, which
	// fingerprints the TLS/HTTP2 handshake. WinHTTP's handshake is trusted on some
	// networks but challenged on others (a 403 / "Just a moment..." page).
	// curl-impersonate forges a real Chrome fingerprint and gets through.
	//
	// Pages themselves come through the signed-in WebView2 session (KHWebSession);
	// this transport carries the plain downloads: album covers. The ranged audio
	// prefix for the classifier stays on WinHTTP. libcurl-impersonate.dll (+ zlib.dll)
	// ships next to the exe, is loaded lazily, and is optional: without it every
	// download uses WinHTTP.

	// curl option / info constants (ABI-stable; see curl/curl.h). Hand-declared
	// so the build needs no curl include path.
	enum : int {
		KHCURLOPT_URL               = 10002,
		KHCURLOPT_WRITEFUNCTION     = 20011,
		KHCURLOPT_WRITEDATA         = 10001,
		KHCURLOPT_FOLLOWLOCATION    = 52,
		KHCURLOPT_ACCEPT_ENCODING   = 10102,
		KHCURLOPT_SSL_OPTIONS       = 216,
		KHCURLOPT_TIMEOUT_MS        = 155,
		KHCURLOPT_CONNECTTIMEOUT_MS = 156,
		KHCURLOPT_NOSIGNAL          = 99,
		KHCURLOPT_FRESH_CONNECT     = 74,
		KHCURLOPT_NOPROGRESS        = 43,
		KHCURLOPT_XFERINFOFUNCTION  = 20219,
		KHCURLOPT_XFERINFODATA      = 10057,
		KHCURLOPT_PROTOCOLS_STR     = 10318,
		KHCURLOPT_REDIR_PROTOCOLS_STR = 10319,
		KHCURLINFO_RESPONSE_CODE    = 2097154,
		KHCURLSSLOPT_NATIVE_CA      = 16,
		KHCURL_GLOBAL_DEFAULT       = 3,
	};

	// curl-impersonate target: must be a CURRENT Chrome fingerprint. Cloudflare
	// flags stale versions (chrome120/124 are already blocked), so when this
	// starts failing, refresh libcurl-impersonate.dll and bump this string.
#define KH_IMPERSONATE_TARGET "chrome146"
	// Cloudflare challenges intermittently even with a perfect fingerprint, so
	// retry a few times before giving up and falling back to WinHTTP.
#define KH_CURL_ATTEMPTS 3

	typedef void   CURLh;
	typedef int    (*pfn_curl_global_init)(long);
	typedef CURLh* (*pfn_curl_easy_init)(void);
	typedef int    (*pfn_curl_easy_setopt)(CURLh*, int, ...);
	typedef int    (*pfn_curl_easy_perform)(CURLh*);
	typedef int    (*pfn_curl_easy_getinfo)(CURLh*, int, ...);
	typedef void   (*pfn_curl_easy_cleanup)(CURLh*);
	typedef int    (*pfn_curl_easy_impersonate)(CURLh*, const char*, int);

	struct CurlApi {
		HMODULE mod = nullptr;
		bool ok = false;
		pfn_curl_global_init      global_init = nullptr;
		pfn_curl_easy_init        easy_init = nullptr;
		pfn_curl_easy_setopt      easy_setopt = nullptr;
		pfn_curl_easy_perform     easy_perform = nullptr;
		pfn_curl_easy_getinfo     easy_getinfo = nullptr;
		pfn_curl_easy_cleanup     easy_cleanup = nullptr;
		pfn_curl_easy_impersonate easy_impersonate = nullptr;
	};
	static CurlApi g_curl;
	static std::once_flag g_curlOnce;

	template <typename T>
	static T CurlProc(HMODULE m, const char* name)
	{
		return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
	}

	static void LoadCurlOnce()
	{
		// Search the application directory (next to the exe) so the bundled DLL
		// and its zlib.dll dependency resolve, plus System32 for OS imports.
		HMODULE m = LoadLibraryExW(L"libcurl-impersonate.dll", nullptr,
								   LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!m) {
			KHLog::Format(L"curl: libcurl-impersonate.dll not loaded (err %lu) - using WinHTTP", GetLastError());
			return;
		}
		g_curl.global_init      = CurlProc<pfn_curl_global_init>(m, "curl_global_init");
		g_curl.easy_init        = CurlProc<pfn_curl_easy_init>(m, "curl_easy_init");
		g_curl.easy_setopt      = CurlProc<pfn_curl_easy_setopt>(m, "curl_easy_setopt");
		g_curl.easy_perform     = CurlProc<pfn_curl_easy_perform>(m, "curl_easy_perform");
		g_curl.easy_getinfo     = CurlProc<pfn_curl_easy_getinfo>(m, "curl_easy_getinfo");
		g_curl.easy_cleanup     = CurlProc<pfn_curl_easy_cleanup>(m, "curl_easy_cleanup");
		g_curl.easy_impersonate = CurlProc<pfn_curl_easy_impersonate>(m, "curl_easy_impersonate");

		if (g_curl.global_init && g_curl.easy_init && g_curl.easy_setopt && g_curl.easy_perform
				&& g_curl.easy_getinfo && g_curl.easy_cleanup && g_curl.easy_impersonate) {
			g_curl.global_init(KHCURL_GLOBAL_DEFAULT);
			g_curl.mod = m;
			g_curl.ok = true;
			KHLog::Format(L"curl: libcurl-impersonate loaded (target %S)", KH_IMPERSONATE_TARGET);
		} else {
			FreeLibrary(m);
			KHLog::Line(L"curl: libcurl-impersonate.dll missing expected exports - using WinHTTP");
		}
	}

	static bool CurlAvailable()
	{
		std::call_once(g_curlOnce, LoadCurlOnce);
		return g_curl.ok;
	}

	// Where a download goes: memory (covers, the classifier's prefix) or an open
	// file (track audio for the cache). 'cancelled' (optional) ends it early.
	struct Sink {
		urlData* data = nullptr;
		HANDLE file = INVALID_HANDLE_VALUE;
		size_t maxBytes = 0;      // 0 = no limit
		bool truncateOk = false;  // stop quietly at maxBytes instead of failing
		size_t size = 0;
		bool overflow = false;
		bool truncated = false;
		bool writeFailed = false;
		unsigned long long total = 0; // whole file size from a 206 reply's Content-Range
		bool partial = false;         // the reply was 206 Partial Content
		const std::function<bool()>* cancelled = nullptr;

		bool Cancelled() const { return g_shuttingDown || (cancelled && *cancelled && (*cancelled)()); }

		bool Reset()
		{
			size = 0;
			total = 0;
			overflow = truncated = writeFailed = partial = false;
			if (data) {
				data->clear();
			}
			if (file != INVALID_HANDLE_VALUE) {
				LARGE_INTEGER zero = {};
				return SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) && SetEndOfFile(file);
			}
			return true;
		}

		// false ends the transfer: over the cap, a write error, or the cap reached with truncateOk
		bool Put(const char* p, size_t n)
		{
			if (maxBytes && size + n > maxBytes) {
				if (!truncateOk) {
					overflow = true;
					return false;
				}
				n = maxBytes - size;
				truncated = true;
			}
			if (file != INVALID_HANDLE_VALUE) {
				DWORD written = 0;
				if (n && (!WriteFile(file, p, static_cast<DWORD>(n), &written, nullptr) || written != n)) {
					writeFailed = true;
					return false;
				}
			} else if (data) {
				data->insert(data->end(), p, p + n);
			}
			size += n;
			return !truncated;
		}

		void Drop()
		{
			if (data) {
				data->clear();
			}
		}
	};

	static size_t CurlWriteCb(char* ptr, size_t size, size_t nmemb, void* ud)
	{
		const size_t n = size * nmemb;
		return static_cast<Sink*>(ud)->Put(ptr, n) ? n : 0; // 0 aborts the transfer
	}

	static int CurlProgressCb(void* ud, long long, long long, long long, long long)
	{
		return static_cast<Sink*>(ud)->Cancelled() ? 1 : 0; // non-zero aborts the transfer
	}

	// Single GET over libcurl-impersonate into 'sink'. True on a < 400 response
	// with a non-empty body within the sink's cap.
	static bool CurlGet(const CStringW& url, Sink& sink, long timeoutMs, long* pStatus)
	{
		*pStatus = 0;
		if (!sink.Reset()) {
			return false;
		}
		CURLh* h = g_curl.easy_init();
		if (!h) {
			return false;
		}

		const CStringA urlA = WStrToUTF8(url);

		g_curl.easy_setopt(h, KHCURLOPT_URL, urlA.GetString());
		g_curl.easy_setopt(h, KHCURLOPT_PROTOCOLS_STR, "http,https");
		g_curl.easy_setopt(h, KHCURLOPT_REDIR_PROTOCOLS_STR, "http,https");
		g_curl.easy_setopt(h, KHCURLOPT_WRITEFUNCTION, &CurlWriteCb);
		g_curl.easy_setopt(h, KHCURLOPT_WRITEDATA, &sink);
		g_curl.easy_setopt(h, KHCURLOPT_FOLLOWLOCATION, 1L);
		g_curl.easy_setopt(h, KHCURLOPT_ACCEPT_ENCODING, "");                       // advertise + auto-decompress
		g_curl.easy_setopt(h, KHCURLOPT_SSL_OPTIONS, static_cast<long>(KHCURLSSLOPT_NATIVE_CA)); // Windows cert store
		g_curl.easy_setopt(h, KHCURLOPT_NOSIGNAL, 1L);
		g_curl.easy_setopt(h, KHCURLOPT_FRESH_CONNECT, 1L);                         // never reuse a possibly-challenged connection
		g_curl.easy_setopt(h, KHCURLOPT_CONNECTTIMEOUT_MS, 15000L);
		g_curl.easy_setopt(h, KHCURLOPT_TIMEOUT_MS, timeoutMs);
		g_curl.easy_setopt(h, KHCURLOPT_NOPROGRESS, 0L);
		g_curl.easy_setopt(h, KHCURLOPT_XFERINFOFUNCTION, &CurlProgressCb);
		g_curl.easy_setopt(h, KHCURLOPT_XFERINFODATA, &sink);

		// Forge a current Chrome TLS/HTTP2 fingerprint + matching default headers.
		g_curl.easy_impersonate(h, KH_IMPERSONATE_TARGET, 1);

		const int rc = g_curl.easy_perform(h);
		long status = 0;
		g_curl.easy_getinfo(h, KHCURLINFO_RESPONSE_CODE, &status);
		g_curl.easy_cleanup(h);
		*pStatus = status;

		KHLog::Format(L"curl GET %s -> rc=%d status=%ld, %llu bytes%s%s", url.GetString(), rc, status,
			static_cast<unsigned long long>(sink.size), sink.overflow ? L" (over the size cap, dropped)" : L"",
			sink.writeFailed ? L" (could not write the file)" : L"");

		if (rc != 0 || status >= 400 || sink.overflow || sink.writeFailed) {
			sink.Drop();
			return false;
		}
		return sink.size > 0;
	}

	// GET over WinHTTP with browser-like headers, bounded timeouts and an overall
	// deadline. 'range' (optional) asks for part of the file; with the sink's
	// truncateOk reading stops quietly at its cap, otherwise a bigger body fails.
	static bool WinHttpGet(const CStringW& url, Sink& sink, LPCWSTR range, ULONGLONG deadlineMs)
	{
		if (sink.Cancelled() || !sink.Reset()) {
			return false;
		}

		WCHAR host[256] = {};
		WCHAR path[2048] = {};
		URL_COMPONENTSW uc = { sizeof(uc) };
		uc.lpszHostName = host;
		uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
		uc.lpszUrlPath = path;
		uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
		if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
			KHLog::Format(L"WinHttpCrackUrl failed for %s (err %lu)", url.GetString(), GetLastError());
			return false;
		}
		if (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP) {
			return false;
		}

		const ULONGLONG started = GetTickCount64();
		DWORD dwStatus = 0;
		bool timedOut = false;

		if (auto hSession = WinHttpOpen(KH_USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)) {
			// resolve, connect, send, receive: no single step may hang the worker
			WinHttpSetTimeouts(hSession, 10000, 10000, 15000, 20000);
			DWORD dwProto = WINHTTP_PROTOCOL_FLAG_HTTP2;
			WinHttpSetOption(hSession, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &dwProto, sizeof(dwProto));

			if (auto hConnect = WinHttpConnect(hSession, host, uc.nPort, 0)) {
				const DWORD dwFlags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
				if (auto hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
													   KHINSIDER_BASE L"/", WINHTTP_DEFAULT_ACCEPT_TYPES, dwFlags)) {
					// Don't carry cookies or pooled connections between requests.
					DWORD dwDisable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_KEEP_ALIVE;
					WinHttpSetOption(hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &dwDisable, sizeof(dwDisable));

					CStringW headers = L"Accept: */*\r\nAccept-Language: en-US,en;q=0.9\r\n";
					if (range) {
						headers += range;
					}

					if (WinHttpSendRequest(hRequest, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
							&& WinHttpReceiveResponse(hRequest, nullptr)) {
						DWORD dwLen = sizeof(dwStatus);
						WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
											WINHTTP_HEADER_NAME_BY_INDEX, &dwStatus, &dwLen, WINHTTP_NO_HEADER_INDEX);
						if (dwStatus == 206) {
							// "bytes 0-262143/5242880": the size after the slash places the middle window
							sink.partial = true;
							WCHAR cr[128] = {};
							DWORD crLen = sizeof(cr);
							if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, cr, &crLen, WINHTTP_NO_HEADER_INDEX)) {
								if (const wchar_t* slash = wcschr(cr, L'/')) {
									sink.total = _wcstoui64(slash + 1, nullptr, 10);
								}
							}
						}

						std::vector<char> tmp(64 * 1024);
						while (dwStatus < 400) {
							if (sink.Cancelled() || GetTickCount64() - started > deadlineMs) {
								timedOut = true;
								break;
							}
							DWORD dwSizeRead = 0;
							if (!WinHttpReadData(hRequest, tmp.data(), static_cast<DWORD>(tmp.size()), &dwSizeRead) || !dwSizeRead) {
								break;
							}
							if (!sink.Put(tmp.data(), dwSizeRead)) {
								break;
							}
						}
					} else {
						KHLog::Format(L"GET %s: send/receive failed (err %lu)", url.GetString(), GetLastError());
					}
					WinHttpCloseHandle(hRequest);
				}
				WinHttpCloseHandle(hConnect);
			}
			WinHttpCloseHandle(hSession);
		}

		KHLog::Format(L"GET %s -> status %lu, %llu bytes%s%s%s", url.GetString(), dwStatus, static_cast<unsigned long long>(sink.size),
			sink.overflow ? L" (over the size cap, dropped)" : L"", sink.writeFailed ? L" (could not write the file)" : L"",
			timedOut ? L" (gave up: deadline, exit or cancelled)" : L"");

		if (dwStatus >= 400 || dwStatus == 0 || sink.overflow || sink.writeFailed || timedOut) {
			sink.Drop();
			return false;
		}
		return sink.size > 0;
	}

	// Plain downloads. Cloudflare-fronted hosts go through curl-impersonate first
	// (retried, since Cloudflare challenges even a real browser now and then),
	// falling back to WinHTTP; ranged requests always use WinHTTP.
	static bool HttpGet(const CStringW& url, Sink& sink, LPCWSTR range = nullptr, long curlTimeoutMs = 30000, ULONGLONG winHttpDeadlineMs = 45000)
	{
		if (!range && !sink.truncateOk && CurlAvailable()) {
			for (int attempt = 1; attempt <= KH_CURL_ATTEMPTS && !sink.Cancelled(); ++attempt) {
				long status = 0;
				if (CurlGet(url, sink, curlTimeoutMs, &status)) {
					return true;
				}
				if (status == 404 || sink.overflow || sink.writeFailed) {
					return false; // retrying won't help
				}
				if (attempt < KH_CURL_ATTEMPTS && !WaitOrCancel(static_cast<DWORD>(400 * attempt))) {
					return false;
				}
			}
			if (sink.Cancelled()) {
				return false;
			}
			KHLog::Line(L"curl: all attempts failed - falling back to WinHTTP");
		}
		return WinHttpGet(url, sink, range, winHttpDeadlineMs);
	}

	static bool HttpGetBytes(const CStringW& url, urlData& data, size_t maxBytes, bool truncateOk = false, LPCWSTR range = nullptr)
	{
		Sink sink;
		sink.data = &data;
		sink.maxBytes = maxBytes;
		sink.truncateOk = truncateOk;
		return HttpGet(url, sink, range);
	}

	static bool FetchPage(const CStringW& url, const CStringA& body, std::string& html, CStringW* finalUrl, FetchStatus* status)
	{
		FetchStatus result = FetchStatus::NetworkError;
		if (!g_shuttingDown) {
			auto response = KHWebSession::Request(url.GetString(), body.GetString());
			switch (response.status) {
				case KHWebSession::Status::Ready: result = FetchStatus::Success; break;
				case KHWebSession::Status::LoginRequired: result = FetchStatus::LoginRequired; break;
				case KHWebSession::Status::Blocked: result = FetchStatus::Blocked; break;
				case KHWebSession::Status::Unavailable: result = FetchStatus::BrowserUnavailable; break;
				case KHWebSession::Status::LayoutChanged: result = FetchStatus::LayoutChanged; break;
				default: result = FetchStatus::NetworkError; break;
			}
			if (result == FetchStatus::Success) {
				html = std::move(response.html);
				if (finalUrl) {
					*finalUrl = response.url.c_str();
				}
			} else {
				KHLog::Format(L"page %s -> not fetched (session status %d, HTTP %u)", url.GetString(),
					static_cast<int>(response.status), response.httpStatus);
			}
		}
		if (status) {
			*status = result;
		}
		return result == FetchStatus::Success;
	}

	static bool ParseAlbumChecked(const std::string& html, const CStringW& url, Album& album, FetchStatus* pStatus)
	{
		SaveDebugPage(L"album.html", html);
		bool usedFallback = false;
		const bool ok = ParseAlbumPage(html, url, album, &usedFallback);
		if (usedFallback) {
			KHLog::Format(L"album page %s: no songlist table - scanned the whole page (the site's page format may have changed)", url.GetString());
		}
		if (!ok) {
			KHLog::Format(L"album page %s: no track list found in %u bytes - KHInsider's page format appears to have changed",
				url.GetString(), static_cast<unsigned>(html.size()));
			if (pStatus) {
				*pStatus = FetchStatus::LayoutChanged;
			}
			return false;
		}
		const CStringW coverInfo = album.coverUrls.empty() ? CStringW(L"(none)") : album.coverUrls[0];
		KHLog::Format(L"parsed album: '%s', %u tracks, platforms [%s], %u cover candidate(s), first=%s",
			album.title.GetString(), static_cast<unsigned>(album.tracks.size()),
			album.platforms.IsEmpty() ? L"(none)" : album.platforms.GetString(),
			static_cast<unsigned>(album.coverUrls.size()), coverInfo.GetString());
		if (pStatus) {
			*pStatus = FetchStatus::Success;
		}
		return true;
	}

	bool FetchRandomAlbum(const CStringA& formBody, Album& album, FetchStatus* pStatus)
	{
		// Unique query each call so no cache/edge layer can serve a stale
		// (always-identical) "random" album back to us.
		static std::atomic<unsigned> s_nonce{0};
		const unsigned nonce = ++s_nonce;
		CStringW url;
		url.Format(L"%s/random-album-advanced?_=%llu%u", KHINSIDER_BASE, GetTickCount64(), nonce);

		std::string html;
		CStringW finalUrl;
		if (!FetchPage(url, formBody, html, &finalUrl, pStatus)) {
			return false;
		}

		// strip the "?from=random" tail so history entries compare cleanly
		const int query = finalUrl.Find(L'?');
		if (query != -1) {
			finalUrl.Truncate(query);
		}
		return ParseAlbumChecked(html, finalUrl, album, pStatus);
	}

	bool FetchAlbum(const CStringW& albumUrl, Album& album, FetchStatus* pStatus)
	{
		std::string html;
		if (!FetchPage(albumUrl, CStringA(), html, nullptr, pStatus)) {
			return false;
		}
		return ParseAlbumChecked(html, albumUrl, album, pStatus);
	}

	CStringW ResolveTrackAudioUrl(const CStringW& trackPageUrl, FetchStatus* pStatus)
	{
		std::string html;
		if (!FetchPage(trackPageUrl, CStringA(), html, nullptr, pStatus)) {
			return CStringW();
		}
		SaveDebugPage(L"track.html", html);

		const CStringW audio = ExtractTrackAudioUrl(html, PreferredAudioFormat());
		if (audio.IsEmpty()) {
			KHLog::Format(L"no audio URL found on %s - KHInsider's track page format may have changed", trackPageUrl.GetString());
			if (pStatus) {
				*pStatus = FetchStatus::LayoutChanged;
			}
			return CStringW();
		}
		if (!IsAllowedMediaUrl(audio)) {
			KHLog::Format(L"audio link on %s is not on KHInsider's servers - ignored: %s", trackPageUrl.GetString(), audio.GetString());
			if (pStatus) {
				*pStatus = FetchStatus::LayoutChanged;
			}
			return CStringW();
		}
		return audio;
	}

	bool DownloadToFile(const CStringW& url, const CStringW& localPath, size_t maxBytes)
	{
		if (!IsAllowedMediaUrl(url)) {
			KHLog::Format(L"download refused, not a KHInsider host: %s", url.GetString());
			return false;
		}
		urlData data;
		if (!HttpGetBytes(url, data, maxBytes) || data.empty()) {
			return false;
		}
		// plain Win32 writes: a full disk fails the download instead of throwing
		HANDLE h = CreateFileW(localPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			KHLog::Format(L"download: could not create %s (error %lu)", localPath.GetString(), GetLastError());
			return false;
		}
		DWORD written = 0;
		const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
		const DWORD err = GetLastError();
		CloseHandle(h);
		if (!ok) {
			KHLog::Format(L"download: could not write %s (error %lu)", localPath.GetString(), err);
			DeleteFileW(localPath);
		}
		return ok;
	}

	bool DownloadAudioToFile(const CStringW& url, const CStringW& localPath, size_t maxBytes, const std::function<bool()>& cancelled)
	{
		constexpr long kAudioTimeoutMs = 240000; // a whole track, not a page
		if (!IsAllowedMediaUrl(url)) {
			KHLog::Format(L"download refused, not a KHInsider host: %s", url.GetString());
			return false;
		}
		HANDLE h = CreateFileW(localPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			KHLog::Format(L"download: could not create %s (error %lu)", localPath.GetString(), GetLastError());
			return false;
		}
		Sink sink;
		sink.file = h;
		sink.maxBytes = maxBytes;
		sink.cancelled = &cancelled;
		const bool ok = HttpGet(url, sink, nullptr, kAudioTimeoutMs, kAudioTimeoutMs) && !sink.Cancelled();
		CloseHandle(h);
		if (!ok) {
			DeleteFileW(localPath);
		}
		return ok;
	}

	static bool IsMp3Url(const CStringW& url)
	{
		CStringW path = url;
		const int q = path.FindOneOf(L"?#");
		if (q != -1) {
			path.Truncate(q);
		}
		return path.GetLength() > 4 && path.Right(4).CompareNoCase(L".mp3") == 0;
	}

	// Two windows of the mp3 (after the ID3 tag, and the middle), fetched with
	// ranged GETs of 256 KB each, judged by KHAudio. Anything that can't be
	// fetched or decoded comes back not judged, which counts as music.
	KHAudio::Decision ClassifyTrackAudio(const CStringW& audioUrl)
	{
		if (!IsMp3Url(audioUrl) || !IsAllowedMediaUrl(audioUrl)) {
			return {}; // only mp3 is fetched here; ogg is judged by the cache's decode
		}
		const KHAudio::RangeFetch fetch = [&audioUrl](unsigned long long from, unsigned long long to, KHAudio::RangeReply& reply) {
			CStringW range;
			range.Format(L"Range: bytes=%llu-%llu\r\n", from, to);
			Sink sink;
			sink.data = &reply.data;
			sink.maxBytes = static_cast<size_t>(to - from + 1);
			sink.truncateOk = true;
			if (!HttpGet(audioUrl, sink, range)) {
				return false;
			}
			reply.total = sink.total;
			reply.partial = sink.partial;
			return true;
		};
		return KHAudio::ClassifyMp3(fetch);
	}

	KHAudio::Decision ClassifyMonoPcm(const std::vector<float>& first, const std::vector<float>& middle, int hz)
	{
		return KHAudio::ClassifyPcmWindows(first, middle, hz);
	}
}
