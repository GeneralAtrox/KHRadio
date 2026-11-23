/*
 * (C) 2026 MPC-BE KH Radio edition
 * Adapted for the standalone KH Radio app: the MPC-BE docking bar wrapper is
 * gone and playback goes through CRadioPlayer instead of MPC-BE's playlist.
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-BE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdafx.h"
#include "KHRadioDlg.h"
#include "KHWebSession.h"
#include "KHRadioLayout.h"
#include "AppData.h"
#include "Player.h"
#include "UiKit.h"

#include <functional>

// option tables mirroring https://downloads.khinsider.com/random-album-advanced

struct KHOption {
	int value;
	LPCWSTR name;
};

static const KHOption s_khTypes[] = {
	{ 0, L"Unknown"      },
	{ 2, L"Gamerips"     },
	{ 1, L"Soundtracks"  },
	{ 3, L"Singles"      },
	{ 5, L"Arrangements" },
	{ 4, L"Remixes"      },
	{ 6, L"Compilations" },
	{ 7, L"Inspired By"  },
};

static const KHOption s_khPlatforms[] = {
	{  0, L"Unknown"              },
	{ 56, L"3DO"                  },
	{ 15, L"3DS"                  },
	{ 24, L"Amiga"                },
	{ 45, L"Android"              },
	{ 49, L"Anime"                },
	{ 23, L"Arcade"               },
	{ 25, L"Atari 8-Bit"          },
	{ 58, L"Atari Jaguar"         },
	{ 26, L"Atari ST"             },
	{ 28, L"CD-i"                 },
	{ 27, L"Commodore 64"         },
	{ 20, L"Dreamcast"            },
	{ 16, L"DS"                   },
	{ 60, L"Family Computer"      },
	{ 62, L"FDS"                  },
	{ 29, L"FM Towns"             },
	{ 59, L"Fujitsu FM77AV"       },
	{  6, L"Game Gear"            },
	{  3, L"GB"                   },
	{  4, L"GBA"                  },
	{ 19, L"GC"                   },
	{  8, L"Genesis / Mega Drive" },
	{ 63, L"IBM PC"               },
	{ 61, L"IBM PC/AT"            },
	{ 46, L"iOS"                  },
	{ 54, L"Linux"                },
	{ 53, L"MacOS"                },
	{  7, L"Master System"        },
	{ 41, L"Mobile"               },
	{ 50, L"Movie"                },
	{ 30, L"MS-DOS"               },
	{  5, L"MSX"                  },
	{ 31, L"MSX2"                 },
	{ 10, L"N64"                  },
	{ 52, L"Neo Geo"              },
	{  1, L"NES"                  },
	{ 51, L"Online"               },
	{ 32, L"PC-88"                },
	{ 33, L"PC-98"                },
	{ 34, L"PC-9821"              },
	{ 48, L"PC-FX"                },
	{ 42, L"PS Vita"              },
	{  9, L"PS1"                  },
	{ 11, L"PS2"                  },
	{ 12, L"PS3"                  },
	{ 44, L"PS4"                  },
	{ 57, L"PS5"                  },
	{ 13, L"PSP"                  },
	{ 21, L"Saturn"               },
	{ 35, L"Sharp X1"             },
	{  2, L"SNES"                 },
	{ 36, L"Spectrum"             },
	{ 65, L"Stadia"               },
	{ 69, L"Steam"                },
	{ 43, L"Switch"               },
	{ 68, L"Switch 2"             },
	{ 22, L"TurboGrafx-16"        },
	{ 55, L"Virtual Boy"          },
	{ 66, L"VR"                   },
	{ 14, L"Wii"                  },
	{ 39, L"Wii U"                },
	{ 37, L"Windows"              },
	{ 38, L"X68000"               },
	{ 17, L"Xbox"                 },
	{ 18, L"Xbox 360"             },
	{ 47, L"Xbox One"             },
	{ 64, L"Xbox Series X/S"      },
};

#define KHRADIO_YEAR_MAX 2026
#define KHRADIO_YEAR_MIN 1980

// total albums to keep in the queue (the one playing + ones queued ahead)
#define KHRADIO_QUEUE_DEPTH 2

// "Filter non-music tracks" drops tracks shorter than this (seconds)
#define KHRADIO_MIN_TRACK_SEC 30

// one-shot timer fired shortly after boot: if there's nothing to resume,
// auto-start a fresh random album so the user never has to press the button.
#define KHRADIO_AUTOSTART_TIMER 0xA1
#define KHRADIO_AUTOSTART_DELAY 1500

// worker -> UI message payloads

struct KHRadioTextMsg {
	UINT gen;
	CStringW text;
};

struct KHRadioDoneMsg {
	UINT gen;
	bool bAppend;
	int resolved;       // number of tracks queued by this fetch
	int skippedFiltered; // tracks dropped (spoken / short / silent)
	CStringW text;      // error/status to show (empty = success)
};

struct KHRadioAlbumMsg {
	UINT gen;
	bool bAppend;
	KHInsider::Album album;
};

struct KHRadioTrackMsg {
	UINT gen;
	bool bAppend;
	int index;
	CStringW name;
	CStringW audioUrl;
	CStringW coverPath;
};

struct KHRadioCoverMsg {
	CStringW albumUrl;
	CStringW coverPath; // empty: no cover could be fetched
};

// params for the background cover-only fetch used for a queued album whose
// cover is missing (e.g. a restored queue after the file was cleaned up)
struct CoverFetchParams {
	HWND hWnd = nullptr;
	CStringW albumUrl;
};

// Covers live in %APPDATA%\KHRadio\covers, one file per album, so a restored
// queue keeps its art and a replayed album needs no second download.
static CStringW CoverPathFor(const CStringW& albumUrl)
{
	CStringW lower = albumUrl;
	lower.MakeLower();
	const size_t h = std::hash<std::wstring>{}(std::wstring(lower.GetString()));
	CStringW path;
	path.Format(L"%scover_%016llx.img", KHRadioData::CoverDir().GetString(), static_cast<unsigned long long>(h));
	return path;
}

static bool FileHasData(const CStringW& path)
{
	WIN32_FILE_ATTRIBUTE_DATA fa = {};
	return GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && (fa.nFileSizeLow || fa.nFileSizeHigh);
}

// try each candidate (full-size first, thumbnails as fallback) until one works
static CStringW DownloadCover(const KHInsider::Album& album)
{
	if (album.coverUrls.empty() || album.url.IsEmpty()) {
		return L"";
	}
	const CStringW path = CoverPathFor(album.url);
	if (FileHasData(path)) {
		return path;
	}
	CStringW part;
	part.Format(L"%s.%lu.part", path.GetString(), GetCurrentThreadId());
	for (const auto& url : album.coverUrls) {
		if (!url.IsEmpty() && KHInsider::DownloadToFile(url, part)) {
			if (MoveFileExW(part, path, MOVEFILE_REPLACE_EXISTING)) {
				return path;
			}
		}
		::DeleteFileW(part);
	}
	return L"";
}

// CKHRadioDlg

CKHRadioDlg::CKHRadioDlg()
	: CDialog(CKHRadioDlg::IDD, nullptr)
{
}

CKHRadioDlg::~CKHRadioDlg()
{
}

BOOL CKHRadioDlg::Create(CWnd* pParent, IKHRadioHost* host)
{
	m_host = host;
	return __super::Create(IDD, pParent);
}

void CKHRadioDlg::DoDataExchange(CDataExchange* pDX)
{
	__super::DoDataExchange(pDX);

	DDX_Control(pDX, IDC_KHRADIO_TYPE_LIST, m_listType);
	DDX_Control(pDX, IDC_KHRADIO_YEAR_LIST, m_listYear);
	DDX_Control(pDX, IDC_KHRADIO_PLATFORM_LIST, m_listPlatform);
	DDX_Control(pDX, IDC_KHRADIO_CLEAR_BUTTON, m_buttonClear);
	DDX_Control(pDX, IDC_KHRADIO_AVOID_CHECK, m_checkAvoidPlayed);
	DDX_Control(pDX, IDC_KHRADIO_SPOKEN_CHECK, m_checkFilterSpoken);
	DDX_Control(pDX, IDC_KHRADIO_EXCLUSIVE_CHECK, m_checkExclusive);
	DDX_Control(pDX, IDC_KHRADIO_RANDOM_BUTTON, m_buttonRandom);
	DDX_Control(pDX, IDC_KHRADIO_LOGIN_BUTTON, m_buttonLogin);
	DDX_Control(pDX, IDC_KHRADIO_STATUS, m_staticStatus);
	DDX_Control(pDX, IDC_KHRADIO_HISTORY_LIST, m_listHistory);
}

BEGIN_MESSAGE_MAP(CKHRadioDlg, CDialog)
	ON_WM_SIZE()
	ON_WM_DESTROY()
	ON_WM_CTLCOLOR()
	ON_WM_ERASEBKGND()
	ON_WM_DRAWITEM()
	ON_WM_TIMER()
	ON_LBN_SELCHANGE(IDC_KHRADIO_TYPE_LIST, OnSelChangeFilters)
	ON_LBN_SELCHANGE(IDC_KHRADIO_YEAR_LIST, OnSelChangeFilters)
	ON_LBN_SELCHANGE(IDC_KHRADIO_PLATFORM_LIST, OnSelChangeFilters)
	ON_BN_CLICKED(IDC_KHRADIO_CLEAR_BUTTON, OnClearFilters)
	ON_BN_CLICKED(IDC_KHRADIO_AVOID_CHECK, OnAvoidPlayedClicked)
	ON_BN_CLICKED(IDC_KHRADIO_SPOKEN_CHECK, OnFilterSpokenClicked)
	ON_BN_CLICKED(IDC_KHRADIO_EXCLUSIVE_CHECK, OnExclusiveClicked)
	ON_BN_CLICKED(IDC_KHRADIO_RANDOM_BUTTON, OnRandomAlbum)
	ON_BN_CLICKED(IDC_KHRADIO_LOGIN_BUTTON, OnLogin)
	ON_MESSAGE(KHWebSession::WM_STATUS, OnSessionStatus)
	ON_LBN_DBLCLK(IDC_KHRADIO_HISTORY_LIST, OnHistoryDblClk)
	ON_MESSAGE(WM_KHRADIO_STATUS, OnKHRadioStatus)
	ON_MESSAGE(WM_KHRADIO_ALBUM, OnKHRadioAlbum)
	ON_MESSAGE(WM_KHRADIO_TRACK, OnKHRadioTrack)
	ON_MESSAGE(WM_KHRADIO_DONE, OnKHRadioDone)
	ON_MESSAGE(WM_KHRADIO_COVER, OnKHRadioCover)
END_MESSAGE_MAP()

BOOL CKHRadioDlg::OnInitDialog()
{
	__super::OnInitDialog();

	PopulateFilterLists();
	RestoreSelections();

	m_history.Load();
	RefreshHistoryList();

	CRect templateRect;
	GetClientRect(templateRect);
	m_templateSize = templateRect.Size();
	m_templateDpi = m_layoutDpi = GetDpiForWindow(m_hWnd);
	GetFont()->GetLogFont(&m_templateFont);
	for (CWnd* child = GetWindow(GW_CHILD); child; child = child->GetNextWindow()) {
		CRect rect;
		child->GetWindowRect(rect);
		ScreenToClient(rect);
		m_layout.push_back({child->m_hWnd, rect});
	}

	ApplyTheme();

	SetStatus(L"Checking KHInsider sign-in...");
	KHWebSession::Initialize(GetParent()->GetSafeHwnd(), m_hWnd);

	// Radio-only player: once the app has settled, if there's nothing to resume
	// (no restored queue), kick off a fresh random album automatically so the
	// user never has to press the button on boot.
	SetTimer(KHRADIO_AUTOSTART_TIMER, KHRADIO_AUTOSTART_DELAY, nullptr);

	return TRUE;
}

void CKHRadioDlg::ApplyTheme()
{
	const auto& p = KHTheme::Current();
	m_bDarkTheme = p.dark;
	m_brushWindow.DeleteObject();
	m_brushWindow.CreateSolidBrush(p.window);
	m_brushList.DeleteObject();
	m_brushList.CreateSolidBrush(p.listBk);

	for (CListBox* pList : { &m_listType, &m_listYear, &m_listPlatform, &m_listHistory }) {
		if (m_bDarkTheme) {
			// drop the light 3D edge for a flat dark look
			pList->ModifyStyleEx(WS_EX_CLIENTEDGE, 0);
			pList->ModifyStyle(0, WS_BORDER);
		} else {
			pList->ModifyStyle(WS_BORDER, 0);
			pList->ModifyStyleEx(0, WS_EX_CLIENTEDGE);
		}
		pList->SetWindowPos(nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
		KHTheme::ApplyToScrollable(pList->m_hWnd);
	}
	for (CButton* pCheck : { &m_checkAvoidPlayed, &m_checkFilterSpoken, &m_checkExclusive }) {
		KHTheme::ApplyToCheckbox(pCheck->m_hWnd);
	}
	m_buttonRandom.SetButtonStyle(m_bDarkTheme ? BS_OWNERDRAW : BS_DEFPUSHBUTTON, FALSE);
	m_buttonClear.SetButtonStyle(m_bDarkTheme ? BS_OWNERDRAW : BS_PUSHBUTTON, FALSE);
	m_buttonLogin.SetButtonStyle(m_bDarkTheme ? BS_OWNERDRAW : BS_PUSHBUTTON, FALSE);
	RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void CKHRadioDlg::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == KHRADIO_AUTOSTART_TIMER) {
		KillTimer(KHRADIO_AUTOSTART_TIMER); // one-shot

		// If radio is already running (a restored queue is playing) or a fetch
		// is in flight, there's nothing to do.
		if (m_bRadioActive || m_bFetching || KHWebSession::GetStatus() != KHWebSession::Status::Ready) {
			return;
		}

		// Only auto-start a fresh album when the queue is truly empty (e.g.
		// first-ever launch); a restored queue resumes by itself.
		if (m_host && m_host->Player().Queue().empty()) {
			KHInsider::DebugLog(L"radio: nothing to resume on boot -> auto-starting a random album");
			StartFetch(false, L"", false);
		}
		return;
	}

	__super::OnTimer(nIDEvent);
}

BOOL CKHRadioDlg::OnEraseBkgnd(CDC* pDC)
{
	CRect r;
	GetClientRect(r);
	pDC->FillRect(r, &m_brushWindow);
	return TRUE;
}

HBRUSH CKHRadioDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
	const auto& p = KHTheme::Current();
	switch (nCtlColor) {
		case CTLCOLOR_DLG:
		case CTLCOLOR_BTN:
			return m_brushWindow;
		case CTLCOLOR_STATIC:
			pDC->SetTextColor(p.text);
			pDC->SetBkColor(p.window);
			return m_brushWindow;
		case CTLCOLOR_LISTBOX:
			pDC->SetTextColor(p.listText);
			pDC->SetBkColor(p.listBk);
			return m_brushList;
	}

	return __super::OnCtlColor(pDC, pWnd, nCtlColor);
}

void CKHRadioDlg::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct)
{
	if (m_bDarkTheme && (nIDCtl == IDC_KHRADIO_RANDOM_BUTTON || nIDCtl == IDC_KHRADIO_CLEAR_BUTTON || nIDCtl == IDC_KHRADIO_LOGIN_BUTTON)) {
		const auto& p = KHTheme::Current();
		CDC dc;
		dc.Attach(lpDrawItemStruct->hDC);

		CRect r(lpDrawItemStruct->rcItem);
		const bool bPressed = !!(lpDrawItemStruct->itemState & ODS_SELECTED);
		const bool bDisabled = !!(lpDrawItemStruct->itemState & ODS_DISABLED);

		dc.FillSolidRect(r, bPressed ? p.buttonPressed : p.buttonBk);
		dc.Draw3dRect(r, p.buttonBorder, p.buttonBorder);

		CStringW text;
		if (CWnd* pBtn = GetDlgItem(nIDCtl)) {
			pBtn->GetWindowTextW(text);
		}
		dc.SetBkMode(TRANSPARENT);
		dc.SetTextColor(bDisabled ? p.disabled : p.text);
		CFont* pOldFont = dc.SelectObject(GetFont());
		dc.DrawTextW(text, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		dc.SelectObject(pOldFont);

		if (lpDrawItemStruct->itemState & ODS_FOCUS) {
			CRect rf(r);
			rf.DeflateRect(2, 2);
			dc.DrawFocusRect(rf);
		}

		dc.Detach();
		return;
	}

	__super::OnDrawItem(nIDCtl, lpDrawItemStruct);
}

void CKHRadioDlg::OnClearFilters()
{
	m_listType.SetSel(-1, FALSE);
	m_listYear.SetSel(-1, FALSE);
	m_listPlatform.SetSel(-1, FALSE);
	SaveSelections();
}

void CKHRadioDlg::OnDestroy()
{
	(*m_pGen)++; // orphan any running fetch thread
	KHWebSession::Shutdown();
	SaveSelections();

	__super::OnDestroy();
}

void CKHRadioDlg::PopulateFilterLists()
{
	for (const auto& opt : s_khTypes) {
		const int idx = m_listType.AddString(opt.name);
		m_listType.SetItemData(idx, opt.value);
	}

	int idx = m_listYear.AddString(L"Unknown");
	m_listYear.SetItemData(idx, 0);
	for (int year = KHRADIO_YEAR_MAX; year >= KHRADIO_YEAR_MIN; year--) {
		CStringW str;
		str.Format(L"%d", year);
		idx = m_listYear.AddString(str);
		m_listYear.SetItemData(idx, year);
	}

	for (const auto& opt : s_khPlatforms) {
		const int i = m_listPlatform.AddString(opt.name);
		m_listPlatform.SetItemData(i, opt.value);
	}
}

CStringW CKHRadioDlg::JoinSelection(CListBox& list)
{
	CStringW csv;
	for (int i = 0; i < list.GetCount(); i++) {
		if (list.GetSel(i) > 0) {
			if (!csv.IsEmpty()) {
				csv += L",";
			}
			CStringW val;
			val.Format(L"%d", static_cast<int>(list.GetItemData(i)));
			csv += val;
		}
	}
	return csv;
}

void CKHRadioDlg::ApplySelection(CListBox& list, const CStringW& csv)
{
	if (csv.IsEmpty()) {
		return;
	}

	std::vector<int> values;
	int pos = 0;
	CStringW token = csv.Tokenize(L",", pos);
	while (!token.IsEmpty()) {
		values.emplace_back(_wtoi(token));
		token = csv.Tokenize(L",", pos);
	}

	for (int i = 0; i < list.GetCount(); i++) {
		const int data = static_cast<int>(list.GetItemData(i));
		for (const auto v : values) {
			if (v == data) {
				list.SetSel(i, TRUE);
				break;
			}
		}
	}
}

void CKHRadioDlg::GetSelectedValues(CListBox& list, std::vector<int>& values)
{
	values.clear();
	for (int i = 0; i < list.GetCount(); i++) {
		if (list.GetSel(i) > 0) {
			values.emplace_back(static_cast<int>(list.GetItemData(i)));
		}
	}
}

void CKHRadioDlg::RestoreSelections()
{
	const KHRadioData::Settings& s = KHRadioData::Get();

	ApplySelection(m_listType, s.types);
	ApplySelection(m_listYear, s.years);
	ApplySelection(m_listPlatform, s.platforms);
	m_checkAvoidPlayed.SetCheck(s.avoidPlayed ? BST_CHECKED : BST_UNCHECKED);
	m_checkFilterSpoken.SetCheck(s.filterSpoken ? BST_CHECKED : BST_UNCHECKED);
	m_checkExclusive.SetCheck(s.exclusive ? BST_CHECKED : BST_UNCHECKED);

	const auto selCount = [](CListBox& l) {
		int n = 0;
		for (int i = 0; i < l.GetCount(); i++) if (l.GetSel(i) > 0) n++;
		return n;
	};
	CStringW log;
	log.Format(L"restore: types='%s'(%d sel) years='%s'(%d sel) platforms='%s'(%d sel)",
			   s.types.GetString(), selCount(m_listType),
			   s.years.GetString(), selCount(m_listYear),
			   s.platforms.GetString(), selCount(m_listPlatform));
	KHInsider::DebugLog(log);
}

void CKHRadioDlg::SaveSelections()
{
	if (!::IsWindow(m_listType.m_hWnd)) {
		return;
	}
	KHRadioData::Settings& s = KHRadioData::Get();

	s.types = JoinSelection(m_listType);
	s.years = JoinSelection(m_listYear);
	s.platforms = JoinSelection(m_listPlatform);
	s.avoidPlayed = (m_checkAvoidPlayed.GetCheck() == BST_CHECKED);
	s.filterSpoken = (m_checkFilterSpoken.GetCheck() == BST_CHECKED);
	s.exclusive = (m_checkExclusive.GetCheck() == BST_CHECKED);
	KHRadioData::Save();
}

void CKHRadioDlg::OnSelChangeFilters()
{
	SaveSelections();
}

void CKHRadioDlg::OnAvoidPlayedClicked()
{
	SaveSelections();
}

void CKHRadioDlg::OnFilterSpokenClicked()
{
	SaveSelections();
}

void CKHRadioDlg::OnExclusiveClicked()
{
	SaveSelections();
}

void CKHRadioDlg::SetStatus(const CStringW& text)
{
	if (::IsWindow(m_staticStatus.m_hWnd)) {
		m_staticStatus.SetWindowTextW(text);
	}
}

void CKHRadioDlg::RefreshHistoryList()
{
	if (!::IsWindow(m_listHistory.m_hWnd)) {
		return;
	}

	m_listHistory.SetRedraw(FALSE);
	m_listHistory.ResetContent();
	for (const auto& album : m_history.Albums()) {
		CStringW str = album.title;
		if (str.IsEmpty()) {
			str = album.url;
		}
		str.AppendFormat(L"  (%u)", static_cast<unsigned>(album.tracks.size()));
		m_listHistory.AddString(str);
	}
	m_listHistory.SetRedraw(TRUE);
	m_listHistory.Invalidate();
}

void CKHRadioDlg::OnRandomAlbum()
{
	SaveSelections();
	StartFetch(false, L"");
}

void CKHRadioDlg::OnLogin()
{
	KHWebSession::ShowLogin();
}

LRESULT CKHRadioDlg::OnSessionStatus(WPARAM wParam, LPARAM)
{
	const auto status = static_cast<KHWebSession::Status>(wParam);
	m_buttonLogin.SetWindowTextW(status == KHWebSession::Status::Ready ? L"KHInsider account..." : L"Sign in to KHInsider...");
	switch (status) {
		case KHWebSession::Status::Ready:
			if (!m_bFetching) { SetStatus(L"Signed in. Pick your filters and roll an album."); }
			SetTimer(KHRADIO_AUTOSTART_TIMER, KHRADIO_AUTOSTART_DELAY, nullptr);
			break;
		case KHWebSession::Status::Starting: SetStatus(L"Checking KHInsider sign-in..."); break;
		case KHWebSession::Status::LoginRequired:
			m_bRadioActive = false;
			SetStatus(L"Please sign in to KHInsider to continue."); break;
		case KHWebSession::Status::Blocked:
			m_bRadioActive = false;
			SetStatus(L"KHInsider needs browser verification. Open Sign in to continue."); break;
		case KHWebSession::Status::Unavailable:
			SetStatus(L"Browser unavailable. Install Microsoft Edge WebView2 Runtime, then reopen Sign in."); break;
		case KHWebSession::Status::LayoutChanged:
			SetStatus(L"Could not verify KHInsider sign-in: the page format changed."); break;
		default: SetStatus(L"Could not connect to KHInsider. Open Sign in to retry."); break;
	}
	return 0;
}

CSize CKHRadioDlg::GetNaturalSize() const
{
	return CSize(MulDiv(m_templateSize.cx, m_layoutDpi, m_templateDpi),
		MulDiv(m_templateSize.cy, m_layoutDpi, m_templateDpi));
}

// The history list may shrink to this height when the window is short.
static int MinHistoryHeight(UINT dpi)
{
	return MulDiv(64, dpi, 96);
}

int CKHRadioDlg::GetMinimumHeight() const
{
	const CSize natural = GetNaturalSize();
	for (const auto& item : m_layout) {
		if (::GetDlgCtrlID(item.window) == IDC_KHRADIO_HISTORY_LIST) {
			const int historyHeight = MulDiv(item.rect.Height(), m_layoutDpi, m_templateDpi);
			return natural.cy - std::max(0, historyHeight - MinHistoryHeight(m_layoutDpi));
		}
	}
	return natural.cy;
}

void CKHRadioDlg::ScaleForDPI(UINT dpi)
{
	if (!m_hWnd || dpi == m_layoutDpi || m_layout.empty()) { return; }
	m_layoutDpi = dpi;
	LOGFONT font = m_templateFont;
	font.lfHeight = MulDiv(font.lfHeight, dpi, m_templateDpi);
	CFont next;
	if (next.CreateFontIndirectW(&font)) {
		SetFont(&next, FALSE);
		for (const auto& item : m_layout) { ::SendMessageW(item.window, WM_SETFONT, (WPARAM)next.m_hObject, FALSE); }
		m_scaledFont.DeleteObject();
		m_scaledFont.Attach(next.Detach());
	}
	CRect r;
	GetClientRect(r);
	Relayout(r.Width(), r.Height());
}

void CKHRadioDlg::OnSize(UINT type, int cx, int cy)
{
	__super::OnSize(type, cx, cy);
	Relayout(cx, cy);
}

void CKHRadioDlg::Relayout(int cx, int cy)
{
	if (m_layout.empty()) { return; }
	const CSize natural = GetNaturalSize();
	for (const auto& item : m_layout) {
		const bool history = ::GetDlgCtrlID(item.window) == IDC_KHRADIO_HISTORY_LIST;
		CRect r = KHRadioLayout::Place(item.rect, m_layoutDpi, m_templateDpi, natural, CSize(cx, cy), history);
		if (history && cy < natural.cy) {
			// short window: the history list gives up height before anything clips
			r.bottom -= std::min<LONG>(natural.cy - cy, std::max<LONG>(0, r.Height() - MinHistoryHeight(m_layoutDpi)));
		}
		::MoveWindow(item.window, r.left, r.top, r.Width(), r.Height(), TRUE);
	}
}

void CKHRadioDlg::OnHistoryDblClk()
{
	const int sel = m_listHistory.GetCurSel();
	if (sel == LB_ERR || sel >= static_cast<int>(m_history.Albums().size())) {
		return;
	}

	StartFetch(true, m_history.Albums()[sel].url);
}

void CKHRadioDlg::StartFetch(bool bDirect, const CStringW& albumUrl, bool bAppend)
{
	if (KHWebSession::GetStatus() != KHWebSession::Status::Ready) {
		OnSessionStatus((WPARAM)KHWebSession::GetStatus(), 0);
		return;
	}
	auto p = std::make_unique<FetchParams>();
	p->hWnd = m_hWnd;
	p->gen = ++(*m_pGen);
	p->pGen = m_pGen;
	p->bDirect = bDirect;
	p->bAppend = bAppend;
	p->albumUrl = albumUrl;
	p->bAvoidPlayed = (m_checkAvoidPlayed.GetCheck() == BST_CHECKED);
	p->bFilterSpoken = (m_checkFilterSpoken.GetCheck() == BST_CHECKED);
	p->bExclusive = (m_checkExclusive.GetCheck() == BST_CHECKED);

	if (!bDirect) {
		std::vector<int> types, years, platforms;
		GetSelectedValues(m_listType, types);
		GetSelectedValues(m_listYear, years);
		GetSelectedValues(m_listPlatform, platforms);
		p->formBody = KHInsider::BuildRandomAlbumForm(types, years, platforms);

		// exclusive mode compares each album's platform tag against the chosen
		// consoles' display names (which match KHInsider's own platform labels)
		for (int i = 0; i < m_listPlatform.GetCount(); i++) {
			if (m_listPlatform.GetSel(i) > 0) {
				CStringW name;
				m_listPlatform.GetText(i, name);
				p->selectedPlatformNames.emplace_back(name);
			}
		}

		for (const auto& album : m_history.Albums()) {
			p->playedAlbums.emplace_back(album.url);
		}
	}

	if (!bAppend) {
		// a replace fetch empties the queue, so the radio restarts with it
		m_currentPlayingAlbum.Empty();
	}

	m_bFetching = true;
	m_bPlaylistStarted = false;
	SetStatus(bAppend ? L"Queuing the next album..." : (bDirect ? L"Loading album..." : L"Rolling a random album..."));

	if (AfxBeginThread(FetchThreadProc, p.get())) {
		p.release(); // owned by the thread now
	} else {
		m_bFetching = false;
		SetStatus(L"Could not start the fetch thread.");
	}
}

int CKHRadioDlg::CountQueuedAlbums() const
{
	if (!m_host) {
		return 0;
	}
	const auto& queue = m_host->Player().Queue();
	const int start = std::max(0, m_host->Player().CurrentIndex());
	int albums = 0;
	CStringW last;
	for (size_t i = start; i < queue.size(); i++) {
		if (albums == 0 || queue[i].ref.albumUrl.CompareNoCase(last) != 0) {
			albums++;
			last = queue[i].ref.albumUrl;
		}
	}
	return albums;
}

void CKHRadioDlg::MaybePrefetchNext()
{
	// keep KHRADIO_QUEUE_DEPTH albums in the queue so the next one is ready
	// well before the current album ends. Only one fetch runs at a time.
	const int queued = CountQueuedAlbums();
	const bool go = m_bRadioActive && !m_bFetching && queued < KHRADIO_QUEUE_DEPTH;
	CStringW lg;
	lg.Format(L"prefetch-check: active=%d fetching=%d queued=%d -> %s",
			  m_bRadioActive ? 1 : 0, m_bFetching ? 1 : 0, queued, go ? L"FETCH" : L"skip");
	KHInsider::DebugLog(lg);
	if (go) {
		StartFetch(false, L"", true);
	}
}

void CKHRadioDlg::StartCoverFetch(const CStringW& albumUrl)
{
	if (albumUrl.IsEmpty()) {
		return;
	}
	for (const auto& pending : m_coverFetches) {
		if (pending.CompareNoCase(albumUrl) == 0) {
			return; // already on its way
		}
	}
	auto p = std::make_unique<CoverFetchParams>();
	p->hWnd = m_hWnd;
	p->albumUrl = albumUrl;
	if (AfxBeginThread(CoverFetchThreadProc, p.get())) {
		p.release();
		m_coverFetches.push_back(albumUrl);
	}
}

UINT CKHRadioDlg::CoverFetchThreadProc(LPVOID pParam)
{
	std::unique_ptr<CoverFetchParams> p(static_cast<CoverFetchParams*>(pParam));

	auto* msg = new KHRadioCoverMsg{ p->albumUrl, L"" };
	KHInsider::Album album;
	if (KHInsider::FetchAlbum(p->albumUrl, album, nullptr)) {
		album.url = p->albumUrl;
		msg->coverPath = DownloadCover(album);
	}
	if (!::PostMessageW(p->hWnd, WM_KHRADIO_COVER, 0, reinterpret_cast<LPARAM>(msg))) {
		delete msg;
	}
	return 0;
}

// True if every console the album is tagged with is among the user's selected
// consoles - i.e. the album is exclusive to the chosen platform(s). An empty
// album tag can't be verified, so it's treated as exclusive (fail-open) to
// avoid dead-ending the radio if a page's platform line ever fails to parse.
static bool AlbumExclusiveTo(const CStringW& albumPlatforms, const std::vector<CStringW>& selected)
{
	if (albumPlatforms.IsEmpty() || selected.empty()) {
		return true;
	}
	int pos = 0;
	while (pos < albumPlatforms.GetLength()) {
		const int comma = albumPlatforms.Find(L',', pos);
		CStringW tok = (comma < 0) ? albumPlatforms.Mid(pos) : albumPlatforms.Mid(pos, comma - pos);
		tok.Trim();
		if (!tok.IsEmpty()) {
			bool inSel = false;
			for (const auto& s : selected) {
				if (s.CompareNoCase(tok) == 0) { inSel = true; break; }
			}
			if (!inSel) {
				return false; // tagged for a console the user didn't pick
			}
		}
		if (comma < 0) {
			break;
		}
		pos = comma + 1;
	}
	return true;
}

UINT CKHRadioDlg::FetchThreadProc(LPVOID pParam)
{
	std::unique_ptr<FetchParams> p(static_cast<FetchParams*>(pParam));

	const auto cancelled = [&]() {
		return (*p->pGen) != p->gen;
	};
	const auto postText = [&](UINT msg, const CStringW& text) {
		auto* m = new KHRadioTextMsg{ p->gen, text };
		if (!::PostMessageW(p->hWnd, msg, 0, reinterpret_cast<LPARAM>(m))) {
			delete m;
		}
	};

	constexpr int maxAttempts = 8;
	// exclusive mode rerolls multi-platform albums, so it needs more tries to
	// land on a console-exclusive one
	const int attempts = p->bDirect ? 1 : (p->bExclusive ? 14 : maxAttempts);

	int resolved = 0;
	int skippedFiltered = 0;
	bool fetchOk = false;
	KHInsider::FetchStatus fetchStatus = KHInsider::FetchStatus::NetworkError;

	for (int attempt = 1; attempt <= attempts && !cancelled(); attempt++) {
		KHInsider::Album album;
		const bool gotAlbum = p->bDirect
			? KHInsider::FetchAlbum(p->albumUrl, album, &fetchStatus)
			: KHInsider::FetchRandomAlbum(p->formBody, album, &fetchStatus);
		if (!gotAlbum) {
			break; // network/parse failure; fetchStatus has the reason
		}
		fetchOk = true;

		// skip already-heard albums (random mode only)
		if (!p->bDirect && p->bAvoidPlayed && attempt < attempts) {
			bool bPlayed = false;
			for (const auto& url : p->playedAlbums) {
				if (url.CompareNoCase(album.url) == 0) {
					bPlayed = true;
					break;
				}
			}
			if (bPlayed) {
				CStringW status;
				status.Format(L"Already heard '%s' - rerolling...", album.title.GetString());
				postText(WM_KHRADIO_STATUS, status);
				continue;
			}
		}

		// exclusive-console mode (random only): skip albums tagged for any console
		// the user didn't pick, rerolling until we find a console-exclusive one
		if (!p->bDirect && p->bExclusive && !p->selectedPlatformNames.empty()
				&& !AlbumExclusiveTo(album.platforms, p->selectedPlatformNames)) {
			KHInsider::DebugLog(L"exclusive: '" + album.title + L"' [" + album.platforms + L"] is multi-platform"
								+ (attempt < attempts ? L" - rerolling" : L" - keeping (no exclusive found)"));
			if (attempt < attempts) {
				CStringW status;
				status.Format(L"'%s' is multi-platform - rerolling for an exclusive...", album.title.GetString());
				postText(WM_KHRADIO_STATUS, status);
				continue;
			}
			// last attempt: fall through and accept so the radio never dead-ends
		}

		// whole-album spoken filter: drama / voice albums (e.g. "...Mini Drama",
		// "Drama CD", "Voice Collection") are entirely spoken, so skip the album
		// by title instead of downloading and analyzing each track
		if (!p->bDirect && p->bFilterSpoken && KHInsider::IsSpokenTitle(album.title)) {
			KHInsider::DebugLog(L"spoken-album: '" + album.title + L"' -> rerolling");
			if (attempt < attempts) {
				CStringW status;
				status.Format(L"'%s' is a drama/voice album - rerolling...", album.title.GetString());
				postText(WM_KHRADIO_STATUS, status);
				continue;
			}
		}

		if (album.tracks.empty()) {
			if (!p->bDirect && attempt < attempts) {
				continue;
			}
			break;
		}

		// download the album cover for the now-playing panel
		const CStringW coverPath = DownloadCover(album);

		if (p->bFilterSpoken) {
			CStringW status;
			status.Format(L"Checking '%s' for non-music tracks...", album.title.GetString());
			postText(WM_KHRADIO_STATUS, status);
		}

		// resolve tracks; the album message is posted lazily on the first
		// playable track so a fully-filtered album can be rerolled cleanly
		bool albumPosted = false;
		skippedFiltered = 0; // count for this album only (rerolls reset it)
		for (size_t i = 0; i < album.tracks.size() && !cancelled(); i++) {
			// cheap filter first: drop very short tracks using the page's
			// duration, before spending a request resolving the audio
			if (p->bFilterSpoken && album.tracks[i].durationSec > 0
					&& album.tracks[i].durationSec < KHRADIO_MIN_TRACK_SEC) {
				skippedFiltered++;
				KHInsider::DebugLog(L"skip short track '" + album.tracks[i].name + L"'");
				continue;
			}

			const CStringW audioUrl = KHInsider::ResolveTrackAudioUrl(album.tracks[i].pageUrl, &fetchStatus);
			if (fetchStatus == KHInsider::FetchStatus::LoginRequired || fetchStatus == KHInsider::FetchStatus::Blocked
					|| fetchStatus == KHInsider::FetchStatus::BrowserUnavailable) {
				fetchOk = false;
				break;
			}
			if (audioUrl.IsEmpty()) {
				continue;
			}
			if (p->bFilterSpoken) {
				const auto verdict = KHInsider::ClassifyTrackAudio(audioUrl, album.tracks[i].name);
				if (verdict != KHInsider::AudioVerdict::Music) {
					skippedFiltered++;
					continue;
				}
			}

			if (!albumPosted) {
				auto* am = new KHRadioAlbumMsg{ p->gen, p->bAppend, album };
				if (!::PostMessageW(p->hWnd, WM_KHRADIO_ALBUM, 0, reinterpret_cast<LPARAM>(am))) {
					delete am;
					return 0;
				}
				albumPosted = true;
			}

			auto* m = new KHRadioTrackMsg{ p->gen, p->bAppend, static_cast<int>(i), album.tracks[i].name, audioUrl, coverPath };
			if (!::PostMessageW(p->hWnd, WM_KHRADIO_TRACK, 0, reinterpret_cast<LPARAM>(m))) {
				delete m;
				return 0;
			}
			resolved++;
		}

		if (cancelled()) {
			return 0;
		}

		if (!fetchOk || resolved > 0) {
			break; // got a playable album
		}

		// nothing playable (all spoken or unresolved) - reroll if random
		if (!p->bDirect && attempt < attempts) {
			postText(WM_KHRADIO_STATUS, L"Only spoken tracks here - rerolling...");
			continue;
		}
		break;
	}

	if (cancelled()) {
		return 0;
	}

	// build a diagnostic message so site/network breakage is self-explanatory
	CStringW doneText; // empty == success (handler shows "Playing/Queued")
	if (!fetchOk) {
		switch (fetchStatus) {
			case KHInsider::FetchStatus::LoginRequired:
				doneText = L"Please sign in to KHInsider again to continue.";
				break;
			case KHInsider::FetchStatus::BrowserUnavailable:
				doneText = L"KHInsider browser unavailable. Open Sign in to retry.";
				break;
			case KHInsider::FetchStatus::Blocked:
				doneText = L"KHInsider needs browser verification. Open Sign in to continue.";
				break;
			case KHInsider::FetchStatus::LayoutChanged:
				doneText = L"Couldn't read the album page - the site layout may have changed.";
				break;
			default:
				doneText = L"Network error - check your connection.";
				break;
		}
	} else if (resolved == 0) {
		doneText = skippedFiltered > 0
			? CStringW(L"Every track was filtered out - try unchecking 'Filter non-music tracks'.")
			: CStringW(L"No playable tracks found - try different filters.");
	}

	auto* dm = new KHRadioDoneMsg{ p->gen, p->bAppend, resolved, skippedFiltered, doneText };
	if (!::PostMessageW(p->hWnd, WM_KHRADIO_DONE, 0, reinterpret_cast<LPARAM>(dm))) {
		delete dm;
	}

	return 0;
}

LRESULT CKHRadioDlg::OnKHRadioStatus(WPARAM wParam, LPARAM lParam)
{
	std::unique_ptr<KHRadioTextMsg> m(reinterpret_cast<KHRadioTextMsg*>(lParam));
	if (m && m->gen == (*m_pGen)) {
		SetStatus(m->text);
	}
	return 0;
}

LRESULT CKHRadioDlg::OnKHRadioAlbum(WPARAM wParam, LPARAM lParam)
{
	std::unique_ptr<KHRadioAlbumMsg> m(reinterpret_cast<KHRadioAlbumMsg*>(lParam));
	if (!m || m->gen != (*m_pGen)) {
		return 0;
	}

	m_currentAlbum = m->album;
	m_bPlaylistStarted = false;

	CStringW status;
	status.Format(m->bAppend ? L"Up next: %s (%u tracks)" : L"Found: %s (%u tracks) - starting...",
				  m_currentAlbum.title.GetString(), static_cast<unsigned>(m_currentAlbum.tracks.size()));
	if (!m_currentAlbum.platforms.IsEmpty()) {
		status.AppendFormat(L"\r\nPlatforms: %s", m_currentAlbum.platforms.GetString());
	}
	SetStatus(status);

	return 0;
}

LRESULT CKHRadioDlg::OnKHRadioTrack(WPARAM wParam, LPARAM lParam)
{
	std::unique_ptr<KHRadioTrackMsg> m(reinterpret_cast<KHRadioTrackMsg*>(lParam));
	if (!m || m->gen != (*m_pGen) || !m_host) {
		return 0;
	}

	QueueItem item;
	item.audioUrl = m->audioUrl;
	item.label.Format(L"%02d. %s", m->index + 1, m->name.GetString());
	item.ref = { m->name, m_currentAlbum.url, m_currentAlbum.title, m->coverPath, m_currentAlbum.platforms };

	CRadioPlayer& player = m_host->Player();
	if (!m_bPlaylistStarted) {
		if (m->bAppend) {
			const int at = player.Append(item);
			// Jump-start only if playback genuinely ran dry; a paused or
			// loading player keeps what it is doing.
			if (!player.IsActive()) {
				player.PlayIndex(at);
			}
			m_bPlaylistStarted = true;
		} else {
			// Replace (user rolled a new album): switch now.
			player.Clear();
			const int at = player.Append(item);
			player.PlayIndex(at);
			m_bPlaylistStarted = true;

			CStringW status;
			status.Format(L"Playing: %s", m_currentAlbum.title.GetString());
			SetStatus(status);
		}
	} else {
		const int at = player.Append(item);
		// the queue ran dry while this album was still arriving: carry on with it
		if (!player.IsActive()) {
			player.PlayIndex(at);
		}
	}

	return 0;
}

LRESULT CKHRadioDlg::OnKHRadioDone(WPARAM wParam, LPARAM lParam)
{
	std::unique_ptr<KHRadioDoneMsg> m(reinterpret_cast<KHRadioDoneMsg*>(lParam));
	if (!m || m->gen != (*m_pGen)) {
		return 0;
	}

	m_bFetching = false;

	if (m->resolved > 0) {
		if (!m->bAppend) {
			// a replace fetch starts a radio session
			m_bRadioActive = KHWebSession::GetStatus() == KHWebSession::Status::Ready;
		}
		CStringW lg;
		lg.Format(L"done: append=%d resolved=%d -> queued=%d active=%d",
				  m->bAppend ? 1 : 0, m->resolved, CountQueuedAlbums(), m_bRadioActive ? 1 : 0);
		KHInsider::DebugLog(lg);
		// top the queue up if we're still short
		MaybePrefetchNext();
	} else {
		KHInsider::DebugLog(L"done: fetch produced no tracks (resolved=0)");
	}

	if (!m->text.IsEmpty()) {
		SetStatus(m->text);
	} else if (!m_currentAlbum.title.IsEmpty()) {
		CStringW status;
		status.Format(m->bAppend ? L"Queued next: %s" : L"Playing: %s", m_currentAlbum.title.GetString());
		if (m->skippedFiltered > 0) {
			status.AppendFormat(L"  (skipped %d non-music)", m->skippedFiltered);
		}
		SetStatus(status);
	}

	return 0;
}

LRESULT CKHRadioDlg::OnKHRadioCover(WPARAM wParam, LPARAM lParam)
{
	std::unique_ptr<KHRadioCoverMsg> m(reinterpret_cast<KHRadioCoverMsg*>(lParam));
	if (!m) {
		return 0;
	}
	m_coverFetches.erase(std::remove_if(m_coverFetches.begin(), m_coverFetches.end(),
		[&](const CStringW& u) { return u.CompareNoCase(m->albumUrl) == 0; }), m_coverFetches.end());
	if (m_host && !m->coverPath.IsEmpty()) {
		// the cover arrived after the track started, so show it now instead of
		// waiting for the next album
		m_host->Player().UpdateAlbumCover(m->albumUrl, m->coverPath);
		m_host->ShowAlbumCover(m->albumUrl, m->coverPath);
		KHInsider::DebugLog(L"late cover set: " + m->coverPath);
	}
	return 0;
}

void CKHRadioDlg::OnPlaybackStarted(const QueueItem& item, bool resumed)
{
	if (!m_host) {
		return;
	}
	const TrackRef& track = item.ref;

	// Whatever plays here is a radio stream (the app has no local playback), so
	// a started track - including a queue restored from the previous session -
	// is a live radio session: the queue keeps topping up and the "reached the
	// end" triggers fire instead of the player just stopping.
	if (!m_bRadioActive) {
		m_bRadioActive = true;
		KHInsider::DebugLog(resumed ? L"radio: adopted restored queue on startup -> active" : L"radio: playback started -> active");
	}

	if (!resumed) {
		m_history.RecordTrack(track.albumUrl, track.albumTitle, track.name);
		RefreshHistoryList();
	}

	CStringW status;
	status.Format(L"Now playing: %s - %s", track.albumTitle.GetString(), track.name.GetString());
	if (!track.platforms.IsEmpty()) {
		status.AppendFormat(L"\r\nPlatforms: %s", track.platforms.GetString());
	}
	SetStatus(status);

	if (track.albumUrl != m_currentPlayingAlbum) {
		// a new album just started playing: switch the cover
		m_currentPlayingAlbum = track.albumUrl;
		if (!track.coverPath.IsEmpty() && FileHasData(track.coverPath)) {
			m_host->ShowAlbumCover(track.albumUrl, track.coverPath);
		} else {
			m_host->ShowAlbumCover(track.albumUrl, L"");
			KHInsider::DebugLog(L"radio: fetching cover for " + track.albumUrl);
			StartCoverFetch(track.albumUrl);
		}

		CStringW lg;
		lg.Format(L"radio: now playing album '%s', queued=%d", track.albumTitle.GetString(), CountQueuedAlbums());
		KHInsider::DebugLog(lg);
		MaybePrefetchNext();
	}

	// Robustness against manual navigation: whenever the track that just
	// started is the LAST one in the queue (reached by normal advance OR a
	// manual jump to the end), make sure the next album is being fetched so it
	// gets appended before this track ends.
	if (m_bRadioActive && !m_bFetching && m_host->Player().IsAtEnd()) {
		KHInsider::DebugLog(L"radio: on last track of the queue -> fetching next album");
		StartFetch(false, L"", true);
	}
}

bool CKHRadioDlg::ContinueRadioAtEnd()
{
	if (!m_bRadioActive) {
		return false; // not a radio session - let the player stop normally
	}

	// The queue ran dry (the prefetched album wasn't ready in time, or a fetch
	// failed). Make sure a fetch is running; its first track jump-starts
	// playback on arrival. Either way, tell the player not to stop for good.
	KHInsider::DebugLog(L"radio: queue reached end while active - continuing");
	if (!m_bFetching) {
		StartFetch(false, L"", true); // appended album, plays when it arrives
	}
	return true;
}
