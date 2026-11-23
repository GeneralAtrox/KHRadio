/*
 * KH Radio - the hidden WebView2 browser session that fetches KHInsider pages
 * with the user's own sign-in, and recovers from network and site trouble by itself.
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
#include "Log.h"
#include "RapidJson.h"

#pragma warning(push, 3)
#include <WebView2.h>
#pragma warning(pop)
#include <wrl.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace KHWebSession {
namespace {
constexpr wchar_t Home[] = L"https://downloads.khinsider.com/random-album-advanced";
constexpr wchar_t Login[] = L"https://downloads.khinsider.com/forums/login";
constexpr UINT WM_PUMP = WM_APP + 880;
constexpr UINT WM_NETWORK_UP = WM_APP + 881;
constexpr auto RequestTimeout = std::chrono::seconds(45);
constexpr ULONGLONG PendingTimeoutMs = 40000;
constexpr ULONGLONG StartTimeoutMs = 45000;

std::string Utf8(const std::wstring& text)
{
	const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	std::string result(static_cast<size_t>(std::max(0, n)), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), n, nullptr, nullptr);
	return result;
}

std::wstring Wide(const std::string& text)
{
	const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	std::wstring result(static_cast<size_t>(std::max(0, n)), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), n);
	return result;
}

LPCWSTR StatusName(Status s)
{
	switch (s) {
		case Status::Starting: return L"Starting";
		case Status::Ready: return L"Ready";
		case Status::LoginRequired: return L"LoginRequired";
		case Status::Blocked: return L"Blocked";
		case Status::NetworkError: return L"NetworkError";
		case Status::Unavailable: return L"Unavailable";
		case Status::LayoutChanged: return L"LayoutChanged";
	}
	return L"?";
}

// NotifyNetworkConnectivityHintChange (Windows 10 2004+) is looked up at run
// time; this mirrors NL_NETWORK_CONNECTIVITY_HINT, which the SDK only declares
// for newer NTDDI targets.
struct ConnectivityHint {
	int level; // 3 = internet access, 4 = constrained internet access
	int cost;
	BOOLEAN approachingDataLimit;
	BOOLEAN overDataLimit;
	BOOLEAN roaming;
};
using HintCallback = void (WINAPI*)(PVOID, ConnectivityHint);
using PFN_NotifyHint = DWORD (WINAPI*)(HintCallback, PVOID, BOOLEAN, PHANDLE);
using PFN_CancelNotify = DWORD (WINAPI*)(HANDLE);

void WINAPI OnConnectivityHint(PVOID context, ConnectivityHint hint)
{
	if (hint.level == 3 || hint.level == 4) {
		PostMessageW(static_cast<HWND>(context), WM_NETWORK_UP, 0, 0);
	}
}

struct Pending {
	std::wstring url;
	std::string body;
	bool sent = false;
	bool done = false;
	bool probe = false;
	ULONGLONG started = GetTickCount64();
	Response response;
	std::condition_variable cv;
};

struct Session : std::enable_shared_from_this<Session> {
	HWND window = nullptr;
	HWND notify = nullptr;
	std::atomic<bool> closed{false};
	std::atomic<Status> status{Status::Starting};
	std::mutex mutex;
	std::map<std::string, std::shared_ptr<Pending>> pending;
	ComPtr<ICoreWebView2Controller> controller;
	ComPtr<ICoreWebView2> web;
	bool documentReady = false;
	bool loginRequested = false;
	ULONGLONG initialized = GetTickCount64();
	std::wstring profile;

	// recovery state, UI thread only
	int attempt = 0;            // automatic re-checks since the last success
	ULONGLONG retryAt = 0;      // when the next re-check runs (0 = none scheduled)
	bool needRestart = false;   // the browser itself failed: recreate it, don't just reload
	bool probePending = false;
	int suspicious = 0;         // blocked/login answers to ordinary requests since the last good one
	HANDLE netNotify = nullptr;
	PFN_CancelNotify cancelNotify = nullptr;

	void SetStatus(Status next, LPARAM extra = 0, bool force = false) {
		if (closed) { return; }
		const Status prev = status.exchange(next);
		if (prev != next || force) {
			KHLog::Format(L"session: %s -> %s%s", StatusName(prev), StatusName(next),
				extra && next != Status::Starting ? L" (re-check scheduled)" : L"");
			PostMessageW(notify, WM_STATUS, static_cast<WPARAM>(next), extra);
		}
	}

	// Ends waiting requests. 'sentOnly' keeps the ones not yet handed to the
	// page, so they go out with the next document instead of failing.
	void Cancel(Status reason, bool sentOnly = false) {
		std::lock_guard<std::mutex> lock(mutex);
		for (auto it = pending.begin(); it != pending.end();) {
			if (sentOnly && !it->second->sent) { ++it; continue; }
			it->second->response.status = reason;
			it->second->done = true;
			it->second->cv.notify_all();
			if (it->second->probe) { probePending = false; }
			it = pending.erase(it);
		}
	}

	// A failure state: schedule the next automatic re-check, except for
	// LoginRequired, which needs the user.
	void EnterFailure(Status reason) {
		if (reason == Status::LoginRequired) {
			retryAt = 0;
			SetStatus(reason, 0, true);
			return;
		}
		const unsigned delay = RetryDelaySeconds(attempt);
		retryAt = GetTickCount64() + delay * 1000ull;
		SetStatus(reason, static_cast<LPARAM>(delay), true);
	}

	void Fail(Status reason) {
		documentReady = false;
		if (reason == Status::Unavailable) { needRestart = true; }
		Cancel(reason);
		EnterFailure(reason);
	}

	std::string Add(const std::shared_ptr<Pending>& request) {
		GUID guid;
		if (FAILED(CoCreateGuid(&guid))) { return {}; }
		wchar_t id[40] = {};
		StringFromGUID2(guid, id, 40);
		std::string key = Utf8(id);
		std::lock_guard<std::mutex> lock(mutex);
		if (closed) { return {}; }
		pending.emplace(key, request);
		PostMessageW(window, WM_PUMP, 0, 0);
		return key;
	}

	void SendProbe() {
		if (probePending || closed) { return; }
		auto probe = std::make_shared<Pending>();
		probe->url = Home;
		probe->probe = true;
		if (!Add(probe).empty()) { probePending = true; }
	}

	void OnProbeResult(Status result) {
		if (result == Status::Ready) {
			if (attempt > 0) { KHLog::Format(L"session: recovered after %d re-check(s)", attempt); }
			attempt = 0;
			retryAt = 0;
			suspicious = 0;
			needRestart = false;
			loginRequested = false;
			// Keep an opened sign-in window visible so the user can answer the
			// browser's Save Password prompt before closing it themselves.
			SetStatus(Status::Ready);
		} else {
			EnterFailure(result);
		}
	}

	void Complete(const std::string& id, Response response) {
		bool probe = false;
		Status result;
		{
			std::lock_guard<std::mutex> lock(mutex);
			auto it = pending.find(id);
			if (it == pending.end()) { return; } // timed out or cancelled
			probe = it->second->probe;
			if (probe && response.status == Status::Ready
					&& response.html.find("randomAdvanced") == std::string::npos) {
				response.status = Status::LayoutChanged;
				KHLog::Line(L"session: the random-album form is missing from the home page - KHInsider's page format appears to have changed");
			}
			it->second->response = std::move(response);
			result = it->second->response.status;
			it->second->done = true;
			it->second->cv.notify_all();
			pending.erase(it);
		}
		if (probe) {
			probePending = false;
			OnProbeResult(result);
		} else if (result == Status::Ready) {
			suspicious = 0;
		} else if (result == Status::LoginRequired || result == Status::Blocked) {
			// One blocked or login-looking answer (a 429, a challenge page) is
			// not proof the session is gone: re-check the home page, which decides.
			++suspicious;
			if ((result == Status::LoginRequired || suspicious >= 2) && documentReady) {
				KHLog::Format(L"session: request answered %s - re-checking the session", StatusName(result));
				SendProbe();
			}
		}
	}

	void Recover() {
		retryAt = 0;
		++attempt;
		initialized = GetTickCount64();
		KHLog::Format(L"session: automatic re-check %d (%s)", attempt, needRestart ? L"restarting the browser" : L"reloading");
		SetStatus(Status::Starting, attempt, true);
		if (!needRestart && web) {
			documentReady = false;
			if (FAILED(web->Navigate(Home))) { Fail(Status::NetworkError); }
			return;
		}
		Restart();
	}

	void Restart() {
		needRestart = false;
		documentReady = false;
		if (controller) { controller->Close(); }
		controller.Reset();
		web.Reset();
		Start();
	}

	// After resume from sleep or when the network comes back: re-check soon
	// instead of waiting out the back-off.
	void RecoverSoon(LPCWSTR why) {
		const Status s = status;
		if (s == Status::NetworkError || s == Status::Blocked || s == Status::Unavailable || s == Status::LayoutChanged) {
			KHLog::Format(L"session: %s - re-checking now", why);
			attempt = 0;
			retryAt = GetTickCount64() + 1500;
		} else if (s == Status::Ready && documentReady) {
			SendProbe();
		}
	}

	void CheckTimeouts() {
		std::vector<std::string> expired;
		const ULONGLONG now = GetTickCount64();
		{
			std::lock_guard<std::mutex> lock(mutex);
			for (const auto& entry : pending) {
				if (now - entry.second->started > PendingTimeoutMs) { expired.push_back(entry.first); }
			}
		}
		for (const auto& id : expired) { Complete(id, {}); }
		if (status == Status::Starting && now - initialized > StartTimeoutMs) {
			KHLog::Line(L"session: no answer from KHInsider within 45 s");
			Fail(Status::NetworkError);
		}
		if (retryAt && now >= retryAt) { Recover(); }
	}

	void Pump() {
		if (!documentReady || !web || closed) { return; }
		std::map<std::string, std::shared_ptr<Pending>> work;
		{
			std::lock_guard<std::mutex> lock(mutex);
			for (auto& entry : pending) {
				if (!entry.second->sent) {
					entry.second->sent = true;
					work.emplace(entry);
				}
			}
		}
		for (const auto& entry : work) {
			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> json(buffer);
			json.StartObject();
			json.Key("id"); json.String(entry.first.c_str());
			json.Key("url"); json.String(Utf8(entry.second->url).c_str());
			json.Key("body"); json.String(entry.second->body.c_str());
			json.EndObject();
			// Fetch runs inside the site's real browser session. Browser cookie
			// scope and redirects apply; credentials are never copied to native HTTP.
			std::string script = "(async(q)=>{const c=new AbortController();"
				"const t=setTimeout(()=>c.abort(),30000);try{"
				"const r=await fetch(q.url,{method:q.body?'POST':'GET',"
				"body:q.body||undefined,headers:q.body?{'Content-Type':'application/x-www-form-urlencoded'}:{},"
				"credentials:'same-origin',cache:'no-store',signal:c.signal});"
				"const html=await r.text();window.chrome.webview.postMessage({id:q.id,status:r.status,url:r.url,html});"
				"}catch(e){window.chrome.webview.postMessage({id:q.id,status:0,url:q.url,html:''});}"
				"finally{clearTimeout(t);}})(" + std::string(buffer.GetString()) + ");";
			const std::weak_ptr<Session> weak = shared_from_this();
			const auto id = entry.first;
			const auto hr = web->ExecuteScript(Wide(script).c_str(),
				Callback<ICoreWebView2ExecuteScriptCompletedHandler>([weak, id](HRESULT result, LPCWSTR) -> HRESULT {
					if (auto s = weak.lock(); s && !s->closed && FAILED(result)) { s->Complete(id, {}); }
					return S_OK;
				}).Get());
			if (FAILED(hr)) { Complete(id, {}); }
		}
	}

	void Resize() {
		RECT rect;
		GetClientRect(window, &rect);
		if (controller) { controller->put_Bounds(rect); }
	}

	void Attach(ICoreWebView2Controller* value) {
		controller = value;
		controller->get_CoreWebView2(&web);
		if (!web) { Fail(Status::Unavailable); return; }
		Resize();
		controller->put_IsVisible(TRUE);
		ComPtr<ICoreWebView2Settings> settings;
		web->get_Settings(&settings);
		if (settings) {
			ComPtr<ICoreWebView2Settings4> settings4;
			if (SUCCEEDED(settings.As(&settings4))) {
				// Saving is opt-in through WebView2's own prompt. Passwords stay
				// in its per-user profile; native code never reads the form.
				settings4->put_IsPasswordAutosaveEnabled(TRUE);
				settings4->put_IsGeneralAutofillEnabled(TRUE);
			}
		}
		const std::weak_ptr<Session> weak = shared_from_this();
		web->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
			[weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
				if (auto s = weak.lock(); s && !s->closed) {
					LPWSTR uri = nullptr;
					args->get_Uri(&uri);
					const bool allowed = uri && IsSiteUrl(uri);
					CoTaskMemFree(uri);
					if (!allowed) { args->put_Cancel(TRUE); return S_OK; }
					s->documentReady = false;
					s->Cancel(Status::NetworkError, true); // in-flight fetches die with the old page
				}
				return S_OK;
			}).Get(), nullptr);
		web->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
			[weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
				if (auto s = weak.lock(); s && !s->closed) {
					BOOL success = FALSE;
					args->get_IsSuccess(&success);
					if (!success) {
						COREWEBVIEW2_WEB_ERROR_STATUS err = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
						args->get_WebErrorStatus(&err);
						KHLog::Format(L"session: page load failed (WebView2 error %d)", static_cast<int>(err));
						s->Fail(Status::NetworkError);
						return S_OK;
					}
					s->documentReady = true;
					s->SendProbe();
					s->Pump();
				}
				return S_OK;
			}).Get(), nullptr);
		web->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
			[weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
				if (auto s = weak.lock(); s && !s->closed) {
					LPWSTR source = nullptr, raw = nullptr;
					args->get_Source(&source);
					const bool allowed = source && IsSiteUrl(source);
					CoTaskMemFree(source);
					if (!allowed || FAILED(args->get_WebMessageAsJson(&raw)) || !raw) { return S_OK; }
					const auto text = Utf8(raw);
					CoTaskMemFree(raw);
					rapidjson::Document json;
					json.Parse(text.c_str());
					if (json.HasParseError() || !json.IsObject()
							|| !json.HasMember("id") || !json["id"].IsString()
							|| !json.HasMember("status") || !json["status"].IsUint()
							|| !json.HasMember("url") || !json["url"].IsString()
							|| !json.HasMember("html") || !json["html"].IsString()) { return S_OK; }
					Response response;
					response.url = Wide(json["url"].GetString());
					response.httpStatus = json["status"].GetUint();
					response.html.assign(json["html"].GetString(), json["html"].GetStringLength());
					response.status = IsSiteUrl(response.url)
						? ClassifyResponse(response.httpStatus, response.html) : Status::NetworkError;
					s->Complete(json["id"].GetString(), std::move(response));
				}
				return S_OK;
			}).Get(), nullptr);
		web->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
			[](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
				args->put_Handled(TRUE);
				return S_OK;
			}).Get(), nullptr);
		web->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
			[weak](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
				if (auto s = weak.lock(); s && !s->closed) {
					KHLog::Line(L"session: the WebView2 browser process failed");
					s->Fail(Status::Unavailable);
				}
				return S_OK;
			}).Get(), nullptr);
		if (FAILED(web->Navigate(loginRequested ? Login : Home))) { Fail(Status::NetworkError); }
	}

	void Start() {
		initialized = GetTickCount64();
		SetStatus(Status::Starting, attempt);
		const std::weak_ptr<Session> weak = shared_from_this();
		const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, profile.c_str(), nullptr,
			Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
				[weak](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
					if (auto s = weak.lock(); s && !s->closed) {
						if (FAILED(result) || !environment) {
							KHLog::Format(L"session: WebView2 environment failed (0x%08lX)", static_cast<unsigned long>(result));
							s->Fail(Status::Unavailable);
							return S_OK;
						}
						const HRESULT create = environment->CreateCoreWebView2Controller(s->window,
							Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
								[weak](HRESULT created, ICoreWebView2Controller* controller) -> HRESULT {
									if (auto s2 = weak.lock(); s2 && !s2->closed) {
										if (FAILED(created) || !controller) { s2->Fail(Status::Unavailable); }
										else { s2->Attach(controller); }
									}
									return S_OK;
								}).Get());
						if (FAILED(create)) { s->Fail(Status::Unavailable); }
					}
					return S_OK;
				}).Get());
		if (FAILED(hr)) {
			KHLog::Format(L"session: WebView2 could not start (0x%08lX)", static_cast<unsigned long>(hr));
			Fail(Status::Unavailable);
		}
	}

	void WatchNetwork() {
		HMODULE ip = LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!ip) { return; }
		auto notifyHint = reinterpret_cast<PFN_NotifyHint>(reinterpret_cast<void*>(GetProcAddress(ip, "NotifyNetworkConnectivityHintChange")));
		cancelNotify = reinterpret_cast<PFN_CancelNotify>(reinterpret_cast<void*>(GetProcAddress(ip, "CancelMibChangeNotify2")));
		if (notifyHint && cancelNotify && notifyHint(&OnConnectivityHint, window, FALSE, &netNotify) != NO_ERROR) {
			netNotify = nullptr;
		}
		if (!netNotify) {
			KHLog::Line(L"session: network change notifications unavailable - relying on timed re-checks");
		}
	}
};

std::shared_ptr<Session> current;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto* s = reinterpret_cast<Session*>(GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		s = static_cast<Session*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
		SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
	}
	if (s && !s->closed) {
		if (message == WM_SIZE && s->window) { s->Resize(); return 0; }
		if (message == WM_PUMP) { s->Pump(); return 0; }
		if (message == WM_TIMER) { s->CheckTimeouts(); return 0; }
		if (message == WM_NETWORK_UP) { s->RecoverSoon(L"network is back"); return 0; }
		if (message == WM_POWERBROADCAST && (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)) {
			s->RecoverSoon(L"resumed from sleep");
			return TRUE;
		}
		if (message == WM_CLOSE) { ShowWindow(window, SW_HIDE); return 0; }
		if (message == WM_DPICHANGED) {
			const auto* r = reinterpret_cast<RECT*>(lParam);
			SetWindowPos(window, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER);
			return 0;
		}
	}
	return DefWindowProcW(window, message, wParam, lParam);
}
}

bool IsSiteUrl(const std::wstring& url)
{
	constexpr wchar_t prefix[] = L"https://downloads.khinsider.com/";
	return url.size() >= _countof(prefix)-1 && _wcsnicmp(url.c_str(), prefix, _countof(prefix)-1) == 0;
}

Status ClassifyResponse(unsigned status, const std::string& html)
{
	std::string lower = html;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
	if (status == 401 || lower.find("to access the website's features, you need to be registered and logged in") != std::string::npos
			|| lower.find("<title>please log in") != std::string::npos
			|| (lower.find("name=\"password\"") != std::string::npos && lower.find("name=\"login\"") != std::string::npos)) {
		return Status::LoginRequired;
	}
	if (status == 403 || status == 429 || lower.find("<title>just a moment") != std::string::npos
			|| lower.find("<title>attention required") != std::string::npos) { return Status::Blocked; }
	return status >= 200 && status < 300 ? Status::Ready : Status::NetworkError;
}

unsigned RetryDelaySeconds(int attempt)
{
	static const unsigned delays[] = { 10, 20, 40, 80, 160, 300 };
	const int last = static_cast<int>(_countof(delays)) - 1;
	return delays[std::clamp(attempt, 0, last)];
}

void Initialize(HWND owner, HWND notify, const std::wstring& profileDir)
{
	if (std::atomic_load(&current)) { return; }
	auto s = std::make_shared<Session>();
	s->notify = notify;
	s->profile = profileDir;
	std::atomic_store(&current, s);
	if (s->profile.empty()) { s->Fail(Status::Unavailable); return; }
	SHCreateDirectoryExW(nullptr, s->profile.c_str(), nullptr);
	WNDCLASSW cls = {};
	cls.lpfnWndProc = WindowProc;
	cls.hInstance = GetModuleHandleW(nullptr);
	cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
	cls.lpszClassName = L"KHRadio.Session";
	RegisterClassW(&cls);
	s->window = CreateWindowExW(WS_EX_APPWINDOW, cls.lpszClassName,
		L"KHInsider sign in - downloads.khinsider.com", WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 900, 720, owner, nullptr, cls.hInstance, s.get());
	if (!s->window) { s->Fail(Status::Unavailable); return; }
	SetTimer(s->window, 1, 1000, nullptr);
	s->WatchNetwork();
	s->Start();
}

void Shutdown()
{
	auto s = std::atomic_exchange(&current, std::shared_ptr<Session>());
	if (!s) { return; }
	s->closed = true;
	if (s->netNotify && s->cancelNotify) { s->cancelNotify(s->netNotify); s->netNotify = nullptr; }
	s->Cancel(Status::Unavailable);
	if (s->controller) { s->controller->Close(); }
	s->web.Reset();
	s->controller.Reset();
	std::lock_guard<std::mutex> lock(s->mutex);
	if (s->window) { DestroyWindow(s->window); s->window = nullptr; }
}

void ShowLogin()
{
	auto s = std::atomic_load(&current);
	if (!s || !s->window) { return; }
	s->loginRequested = true;
	s->retryAt = 0;
	s->attempt = 0;
	ShowWindow(s->window, SW_SHOWNORMAL);
	SetForegroundWindow(s->window);
	const Status st = s->status;
	if (s->needRestart || !s->web) {
		if (st != Status::Starting || s->needRestart) { s->Restart(); }
	} else {
		s->web->Navigate(st == Status::Blocked || st == Status::Ready ? Home : Login);
	}
}

Status GetStatus()
{
	auto s = std::atomic_load(&current);
	return s ? s->status.load() : Status::Unavailable;
}

Response Request(const std::wstring& url, const std::string& body)
{
	auto s = std::atomic_load(&current);
	if (!s || !IsSiteUrl(url)) { return {}; }
	const auto status = s->status.load();
	if (status != Status::Ready && status != Status::Starting) { Response r; r.status = status; return r; }
	auto request = std::make_shared<Pending>();
	request->url = url;
	request->body = body;
	const auto id = s->Add(request);
	if (id.empty()) { return {}; }
	std::unique_lock<std::mutex> lock(s->mutex);
	if (!request->cv.wait_for(lock, RequestTimeout, [&] { return request->done; })) {
		s->pending.erase(id);
		return {};
	}
	return std::move(request->response);
}
}
