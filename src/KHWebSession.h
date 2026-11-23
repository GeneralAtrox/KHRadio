/*
 * KH Radio - the hidden WebView2 browser session that fetches KHInsider pages
 * with the user's own sign-in.
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

#include <windows.h>
#include <string>

// The browser and its profile stay on the UI thread. Only Request is called
// by scraper workers. No passwords or cookies leave the browser profile.
//
// Recovery: a session that ends up NetworkError, Blocked, Unavailable or
// LayoutChanged re-checks KHInsider by itself with a growing delay (10 s up to
// 5 min), and straight away when the network comes back or the PC resumes from
// sleep. LoginRequired waits for the user to sign in. A single blocked or
// login-looking response to an ordinary request does not change the session
// status; it triggers a re-check of the home page, which decides.
namespace KHWebSession {
enum class Status { Starting, Ready, LoginRequired, Blocked, NetworkError, Unavailable, LayoutChanged };

// Posted to the notify window: wParam = Status. lParam = for Starting, the
// recovery attempt (0 = first start); for the failure states, the seconds until
// the next automatic re-check (0 = none: the user has to act).
constexpr UINT WM_STATUS = WM_APP + 855;

struct Response {
	Status status = Status::NetworkError;
	unsigned httpStatus = 0;
	std::wstring url;
	std::string html;
};

void Initialize(HWND owner, HWND notify, const std::wstring& profileDir);
void Shutdown(); // releases every waiting Request at once
void ShowLogin();
Status GetStatus();
Response Request(const std::wstring& url, const std::string& body = {});

bool IsSiteUrl(const std::wstring& url);
Status ClassifyResponse(unsigned status, const std::string& html);
// The growing delay between automatic re-checks (attempt 0, 1, 2, ...), in seconds.
unsigned RetryDelaySeconds(int attempt);
}
