// KH Radio - application: single instance, identity, first-run carry-over.
// GPL-3.0-or-later (see COPYING.txt).
#include "stdafx.h"
#include "AppData.h"
#include "AudioCache.h"
#include "KHInsider.h"
#include "Log.h"
#include "MainWnd.h"
#include "UiKit.h"
#include "Version.h"

#include <shobjidl.h>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

class CKHRadioApp : public CWinApp
{
public:
	CKHRadioApp() : CWinApp(L"KH Radio") {}
	BOOL InitInstance() override;
	int ExitInstance() override;

private:
	HANDLE m_mutex = nullptr;
};

CKHRadioApp theApp;

static void ActivateRunningInstance()
{
	// The first instance may still be creating its window; wait up to ~3 s.
	for (int i = 0; i < 30; i++) {
		if (HWND hwnd = FindWindowW(L"KHRadio", nullptr)) {
			if (IsIconic(hwnd)) {
				ShowWindow(hwnd, SW_RESTORE);
			} else {
				ShowWindow(hwnd, SW_SHOW);
			}
			SetForegroundWindow(hwnd);
			return;
		}
		Sleep(100);
	}
}

BOOL CKHRadioApp::InitInstance()
{
	// Own identity: nothing here is shared with MPC-BE. No file arguments are
	// accepted and nothing is registered with the shell.
	SetCurrentProcessExplicitAppUserModelID(L"KHRadio.Player");

	m_mutex = CreateMutexW(nullptr, FALSE, L"KHRadio");
	if (m_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
		ActivateRunningInstance();
		return FALSE;
	}

	CWinApp::InitInstance();
	if (!AfxOleInit()) { // STA for WebView2, WIC and the media controls
		return FALSE;
	}
	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
	InitCommonControlsEx(&icc);

	KHTheme::InitDarkModeSupport();

	KHLog::SetPath(KHRadioData::LogPath());
	KHLog::Line(L"---- KH Radio " _CRT_WIDE(KHRADIO_VERSION_STR) L" starting ----");
	const auto settingsResult = KHRadioData::Load();
	KHRadioData::RunCarryOver(settingsResult);
	KHRadioData::MigrateCoverCache();
	// Only this instance uses the audio cache (the mutex above): files left by
	// an earlier run that ended badly are spent.
	KHAudioCache::SetDir(KHRadioData::CacheDir());
	KHAudioCache::ClearAll();
	if (!KHRadioData::Get().preferAudioFormat.IsEmpty()) {
		KHInsider::SetPreferredAudioFormat(KHRadioData::Get().preferAudioFormat);
		KHLog::Format(L"settings: tracks in %s are taken first where offered", KHRadioData::Get().preferAudioFormat.GetString());
	}
	if (KHRadioData::Get().saveDebugPages) {
		KHInsider::SetDebugPageDir(KHRadioData::DebugDir());
		KHLog::Format(L"debug: saving the last album and track pages in %s", KHRadioData::DebugDir().GetString());
	}

	auto* wnd = new CMainWnd;
	m_pMainWnd = wnd;
	if (!wnd->CreateMain()) {
		KHLog::Line(L"could not create the main window");
		m_pMainWnd = nullptr;
		return FALSE;
	}
	return TRUE;
}

int CKHRadioApp::ExitInstance()
{
	KHRadioData::Save();
	KHAudioCache::ClearAll();
	KHLog::Line(L"---- KH Radio exiting ----");

	if (m_mutex) {
		CloseHandle(m_mutex);
		m_mutex = nullptr;
	}
	return CWinApp::ExitInstance();
}
