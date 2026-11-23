/*
 * KH Radio - the radio panel: a replica of KHInsider's "Random Album Advanced"
 * form, the listening history, and the workers that fetch albums, look up
 * track audio a few tracks ahead and download covers.
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
#include "KHRadioDlg.h"
#include "KHWebSession.h"
#include "KHRadioLayout.h"
#include "AppData.h"
#include "AudioCache.h"
#include "Log.h"
#include "Player.h"
#include "UiKit.h"

#include <functional>
#include <system_error>

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

#define KHRADIO_YEAR_MIN 1980

// total albums to keep in the queue (the one playing + ones queued ahead)
#define KHRADIO_QUEUE_DEPTH 2

// one-shot timer fired shortly after boot: if there's nothing to resume,
// auto-start a fresh random album so the user never has to press the button.
#define KHRADIO_AUTOSTART_TIMER 0xA1
#define KHRADIO_AUTOSTART_DELAY 1500

// stuck-worker and retry checks
#define KHRADIO_WATCHDOG_TIMER  0xA2
#define KHRADIO_WATCHDOG_MS     5000
#define KHRADIO_FETCH_DEADLINE_MS   180000
#define KHRADIO_RESOLVE_DEADLINE_MS 240000

// audio lookups: how far ahead of the playing track, how many per batch, pacing
#define KHRADIO_RESOLVE_AHEAD 3
#define KHRADIO_RESOLVE_BATCH 3
#define KHRADIO_RESOLVE_PACE_MS 500
#define KHRADIO_RESOLVE_RETRY_MS 10000
#define KHRADIO_RESOLVE_MAX_FAILURES 3

// covers bigger than this are not downloaded
#define KHRADIO_MAX_COVER_BYTES (8u * 1024 * 1024)

// The audio cache (%LOCALAPPDATA%\KHRadio\cache): Ogg tracks are downloaded and
// decoded to WAV there, a few tracks ahead, like the audio lookups.
#define KHRADIO_MAX_AUDIO_DOWNLOAD (64u * 1024 * 1024)    // the .ogg / .m4a file itself
#define KHRADIO_MAX_WAV_BYTES (400ull * 1024 * 1024)      // ~40 min of 44.1 kHz stereo; longer streams
#define KHRADIO_CACHE_CAP (500ull * 1024 * 1024)          // all cache files together
#define KHRADIO_PREPARE_DEADLINE_MS 360000
#define KHRADIO_PREPARE_RETRY_MS 10000
#define KHRADIO_PREPARE_MAX_FAILURES 3

// worker -> UI message payloads

namespace
{
	struct KHRadioTextMsg {
		UINT gen = 0;
		CStringW text;
	};

	struct KHRadioAlbumMsg {
		UINT gen = 0;
		bool bAppend = false;
		bool shared = false;      // a pasted share link asked for this album
		CStringW startAt;         // ... starting at this track (KHShare track key), if any
		KHInsider::Album album;
		std::vector<SkipReason> skips; // per track: why the filter passes over it (short, title)
		CStringW coverPath;       // the cached cover, if it is already on disk
	};

	struct KHRadioDoneMsg {
		UINT gen = 0;
		bool bAppend = false;
		bool failed = false;      // the page fetch failed (status says why)
		KHInsider::FetchStatus status = KHInsider::FetchStatus::Success;
		int queued = 0;           // playable tracks
		int skippedShort = 0;     // tracks the filter passes over: too short
		int skippedTitle = 0;     // ... the title says drama / voice track
		CStringW title;
		CStringW text;            // error to show (empty = success)
	};

	struct KHRadioCoverMsg {
		CStringW albumUrl;
		CStringW coverPath;       // empty: no cover could be fetched
	};

	enum class ResolveOutcome { Resolved, NotFound, Failed, SessionProblem };

	struct KHRadioResolvedMsg {
		UINT gen = 0;
		UINT64 id = 0;
		ResolveOutcome outcome = ResolveOutcome::Failed;
		CStringW audioUrl;
		CStringW name;
		KHAudio::Decision decision; // the talking-track check (not judged when the filter is off)
	};

	struct KHRadioResolveDoneMsg {
		UINT gen = 0;
		bool retryLater = false;
	};

	enum class PrepareOutcome {
		Ready,     // the file is in the cache
		Stream,    // play the address itself (not Vorbis, decode failed, too long)
		NoRoom,    // the cache is full of upcoming tracks: try again later
		Failed,    // the download failed
	};

	struct KHRadioPreparedMsg {
		UINT gen = 0;
		UINT64 id = 0;
		PrepareOutcome outcome = PrepareOutcome::Failed;
		CStringW path;
		CStringW name;
		KHAudio::Decision decision; // the talking-track check of the decoded Ogg
	};

	template <class T>
	void PostOwned(HWND hWnd, UINT msg, std::unique_ptr<T> payload)
	{
		if (::PostMessageW(hWnd, msg, 0, reinterpret_cast<LPARAM>(payload.get()))) {
			payload.release(); // the window owns it now
		}
	}

	template <class T>
	std::unique_ptr<T> TakeOwned(LPARAM lParam)
	{
		return std::unique_ptr<T>(reinterpret_cast<T*>(lParam));
	}

	bool FileHasData(const CStringW& path)
	{
		WIN32_FILE_ATTRIBUTE_DATA fa = {};
		return !path.IsEmpty() && GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && (fa.nFileSizeLow || fa.nFileSizeHigh);
	}

	// Covers live in %LOCALAPPDATA%\KHRadio\covers, one file per album, so a
	// restored queue keeps its art and a replayed album needs no second download.
	CStringW CoverFile(const CStringW& coverDir, const CStringW& albumUrl)
	{
		if (albumUrl.IsEmpty() || coverDir.IsEmpty()) {
			return CStringW();
		}
		CStringW lower = albumUrl;
		lower.MakeLower();
		const size_t h = std::hash<std::wstring>{}(std::wstring(lower.GetString()));
		CStringW path;
		path.Format(L"%scover_%016llx.img", coverDir.GetString(), static_cast<unsigned long long>(h));
		return path;
	}

	// try each candidate (full-size first, thumbnails as fallback) until one works
	CStringW DownloadCover(const KHInsider::Album& album, const CStringW& path)
	{
		if (album.coverUrls.empty() || path.IsEmpty()) {
			return CStringW();
		}
		if (FileHasData(path)) {
			return path;
		}
		CStringW part;
		part.Format(L"%s.%lu.part", path.GetString(), GetCurrentThreadId());
		for (const auto& url : album.coverUrls) {
			if (KHInsider::IsShuttingDown()) {
				break;
			}
			if (!url.IsEmpty() && KHInsider::DownloadToFile(url, part, KHRADIO_MAX_COVER_BYTES)
					&& MoveFileExW(part, path, MOVEFILE_REPLACE_EXISTING)) {
				return path;
			}
			::DeleteFileW(part);
		}
		return CStringW();
	}

	bool Contains(const std::vector<CStringW>& list, const CStringW& value)
	{
		for (const auto& v : list) {
			if (v.CompareNoCase(value) == 0) {
				return true;
			}
		}
		return false;
	}
}

struct CKHRadioDlg::FetchParams {
	HWND hWnd = nullptr;
	UINT gen = 0;
	std::shared_ptr<std::atomic<UINT>> pGen; // shared with the dialog; mismatch = cancelled
	bool bDirect = false;              // true: fetch albumUrl, false: POST the random form
	bool bAppend = false;              // continuous radio: append to the queue instead of replacing it
	CStringA formBody;
	CStringW albumUrl;
	bool bAvoidPlayed = false;
	bool bFilterSpoken = false;
	int minTrackSeconds = 10;           // shorter tracks are skipped while the filter is on
	KHRadioData::KeepList kept;         // tracks the user chose "Play anyway" on
	bool bExclusive = false;            // only keep albums exclusive to the selected console(s)
	bool shared = false;                // a pasted share link
	CStringW startAt;                   // its track (KHShare track key): never filtered out
	std::vector<CStringW> selectedPlatformNames; // display names of the chosen consoles
	std::vector<CStringW> playedAlbums; // snapshot of history URLs
	CStringW coverDir;
};

struct CKHRadioDlg::ResolveParams {
	struct Item {
		UINT64 id = 0;
		CStringW pageUrl;
		CStringW name;
		bool keep = false;   // the shared track, or "Play anyway": the filter leaves it alone
	};
	HWND hWnd = nullptr;
	UINT gen = 0;
	std::shared_ptr<std::atomic<UINT>> pGen;
	bool bFilterSpoken = false;
	std::vector<Item> items;
};

struct CKHRadioDlg::PrepareParams {
	HWND hWnd = nullptr;
	UINT gen = 0;
	std::shared_ptr<std::atomic<UINT>> pGen;
	UINT64 id = 0;
	CStringW url;
	CStringW name;
	QueueItem::Prepare mode = QueueItem::Prepare::DecodeOgg;
	bool current = false;       // the player is waiting for it
	bool bFilterSpoken = false;
};

struct CKHRadioDlg::CoverParams {
	HWND hWnd = nullptr;
	CStringW albumUrl;
	CStringW coverPath;
};

// CKHRadioDlg

CKHRadioDlg::CKHRadioDlg()
	: CDialog(CKHRadioDlg::IDD, nullptr)
{
}

CKHRadioDlg::~CKHRadioDlg()
{
	JoinWorkers(0); // normally already done in OnDestroy
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
	ON_WM_MEASUREITEM()
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
	ON_WM_CONTEXTMENU()
	ON_MESSAGE(WM_KHRADIO_STATUS, OnKHRadioStatus)
	ON_MESSAGE(WM_KHRADIO_ALBUM, OnKHRadioAlbum)
	ON_MESSAGE(WM_KHRADIO_RESOLVED, OnKHRadioResolved)
	ON_MESSAGE(WM_KHRADIO_RESOLVE_DONE, OnKHRadioResolveDone)
	ON_MESSAGE(WM_KHRADIO_PREPARED, OnKHRadioPrepared)
	ON_MESSAGE(WM_KHRADIO_DONE, OnKHRadioDone)
	ON_MESSAGE(WM_KHRADIO_COVER, OnKHRadioCover)
END_MESSAGE_MAP()

BOOL CKHRadioDlg::OnInitDialog()
{
	__super::OnInitDialog();

	PopulateFilterLists();
	RestoreSelections();

	const auto historyResult = m_history.Load(KHRadioData::HistoryPath());
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
	KHWebSession::Initialize(GetParent()->GetSafeHwnd(), m_hWnd, KHRadioData::WebViewProfileDir().GetString());
	if (historyResult == CKHRadioHistory::LoadResult::RecoveredFromBackup) {
		SetStatus(L"The history file was damaged: a copy was kept and the history restored from its backup.");
	} else if (historyResult == CKHRadioHistory::LoadResult::Corrupt) {
		SetStatus(m_history.IsReadOnly()
			? L"The history file is damaged and could not be set aside: history will not be saved this session."
			: L"The history file was damaged: a copy was kept next to it and a new history started.");
	}

	// Radio-only player: once the app has settled, if there's nothing to resume
	// (no restored queue), kick off a fresh random album automatically so the
	// user never has to press the button on boot.
	SetTimer(KHRADIO_AUTOSTART_TIMER, KHRADIO_AUTOSTART_DELAY, nullptr);
	SetTimer(KHRADIO_WATCHDOG_TIMER, KHRADIO_WATCHDOG_MS, nullptr);

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
		if (m_bRadioActive || m_bFetching || m_pendingStart || KHWebSession::GetStatus() != KHWebSession::Status::Ready) {
			return;
		}

		// Only auto-start a fresh album when the queue is truly empty (e.g.
		// first-ever launch); a restored queue resumes by itself.
		if (m_host && m_host->Player().Queue().empty()) {
			KHLog::Line(L"radio: nothing to resume on boot -> auto-starting a random album");
			StartFetch(false, L"", false);
		}
		return;
	}
	if (nIDEvent == KHRADIO_WATCHDOG_TIMER) {
		Watchdog();
		return;
	}

	__super::OnTimer(nIDEvent);
}

void CKHRadioDlg::Watchdog()
{
	const ULONGLONG now = GetTickCount64();
	if (m_bFetching && now - m_fetchStarted > KHRADIO_FETCH_DEADLINE_MS) {
		KHLog::Line(L"watchdog: the album fetch has run for 3 minutes - abandoning it and retrying");
		(*m_pGen)++;
		m_bFetching = false;
		const unsigned delay = ScheduleFetchRetry(KHInsider::FetchStatus::NetworkError);
		CStringW text;
		text.Format(L"KHInsider is not answering. Retrying in %u s.", delay);
		SetStatus(text);
	}
	if (m_bResolving && now - m_resolveStarted > KHRADIO_RESOLVE_DEADLINE_MS) {
		KHLog::Line(L"watchdog: track lookups have run for 4 minutes - abandoning them and retrying");
		(*m_resolveGen)++;
		m_bResolving = false;
	}
	if (m_pendingStart && m_fetchRetryAt && now >= m_fetchRetryAt) {
		RunPendingFetch();
	}
	if (m_resolveRetryAt && now >= m_resolveRetryAt) {
		m_resolveRetryAt = 0;
	}
	if (m_bPreparing && now - m_prepareStarted > KHRADIO_PREPARE_DEADLINE_MS) {
		KHLog::Line(L"watchdog: preparing a track has run for 6 minutes - abandoning it and retrying");
		(*m_prepareGen)++;
		m_bPreparing = false;
		m_prepareId = 0;
	}
	if (m_prepareRetryAt && now >= m_prepareRetryAt) {
		m_prepareRetryAt = 0;
	}
	if (!m_bResolving && m_host && m_host->Player().IsWaitingForResolve()) {
		KickResolver();
	}
	PruneCache();
	KickPreparer();
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

static bool IsRadioList(int id)
{
	return id == IDC_KHRADIO_TYPE_LIST || id == IDC_KHRADIO_YEAR_LIST || id == IDC_KHRADIO_PLATFORM_LIST || id == IDC_KHRADIO_HISTORY_LIST;
}

// The lists are owner-drawn so a selected row uses the palette in both themes:
// the dark-mode listbox theme painted the selection white under white text.
int CKHRadioDlg::ListItemHeight()
{
	CClientDC dc(this);
	CFont* font = GetFont();
	CFont* old = font ? dc.SelectObject(font) : nullptr;
	TEXTMETRICW tm = {};
	dc.GetTextMetricsW(&tm);
	if (old) { dc.SelectObject(old); }
	return std::max<int>(tm.tmHeight + 1, 12);
}

void CKHRadioDlg::OnMeasureItem(int nIDCtl, LPMEASUREITEMSTRUCT lpMeasureItemStruct)
{
	if (IsRadioList(nIDCtl)) {
		lpMeasureItemStruct->itemHeight = static_cast<UINT>(ListItemHeight());
		return;
	}
	__super::OnMeasureItem(nIDCtl, lpMeasureItemStruct);
}

void CKHRadioDlg::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct)
{
	if (IsRadioList(nIDCtl)) {
		const auto& p = KHTheme::Current();
		CDC* dc = CDC::FromHandle(lpDrawItemStruct->hDC);
		CRect r(lpDrawItemStruct->rcItem);
		const bool selected = !!(lpDrawItemStruct->itemState & ODS_SELECTED);
		dc->FillSolidRect(r, selected ? p.selBk : p.listBk);
		if (lpDrawItemStruct->itemID != static_cast<UINT>(-1)) {
			CStringW text;
			const int len = static_cast<int>(::SendMessageW(lpDrawItemStruct->hwndItem, LB_GETTEXTLEN, lpDrawItemStruct->itemID, 0));
			if (len > 0) {
				::SendMessageW(lpDrawItemStruct->hwndItem, LB_GETTEXT, lpDrawItemStruct->itemID, reinterpret_cast<LPARAM>(text.GetBuffer(len + 1)));
				text.ReleaseBuffer();
			}
			dc->SetBkMode(TRANSPARENT);
			dc->SetTextColor(selected ? p.selText : p.listText);
			CRect tr(r);
			tr.DeflateRect(3, 0);
			dc->DrawTextW(text, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		}
		if ((lpDrawItemStruct->itemState & ODS_FOCUS) && !(lpDrawItemStruct->itemState & ODS_NOFOCUSRECT)) {
			dc->DrawFocusRect(r);
		}
		return;
	}
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
	// orphan every running worker, make blocking downloads and page requests
	// give up, then wait a bounded time for the threads to finish
	(*m_pGen)++;
	(*m_resolveGen)++;
	(*m_prepareGen)++;
	KillTimer(KHRADIO_AUTOSTART_TIMER);
	KillTimer(KHRADIO_WATCHDOG_TIMER);
	KHInsider::BeginShutdown();
	KHWebSession::Shutdown();
	JoinWorkers(5000);
	SaveSelections();

	__super::OnDestroy();
}

template <class Fn>
bool CKHRadioDlg::StartWorker(LPCWSTR what, Fn&& fn)
{
	ReapWorkers();
	auto done = std::make_shared<std::atomic<bool>>(false);
	try {
		std::thread thread([done, what, work = std::forward<Fn>(fn)]() mutable {
			try {
				work();
			} catch (CException* e) {
				e->Delete();
				KHLog::Format(L"%s worker: stopped by an MFC exception", what);
			} catch (const std::exception& e) {
				KHLog::Format(L"%s worker: stopped by an exception: %S", what, e.what());
			} catch (...) {
				KHLog::Format(L"%s worker: stopped by an unknown exception", what);
			}
			done->store(true);
		});
		m_workers.push_back({ std::move(thread), done });
		return true;
	} catch (const std::system_error& e) {
		KHLog::Format(L"%s worker: could not start a thread: %S", what, e.what());
		return false;
	}
}

void CKHRadioDlg::ReapWorkers()
{
	for (auto it = m_workers.begin(); it != m_workers.end();) {
		if (it->done->load() && it->thread.joinable()) {
			it->thread.join();
			it = m_workers.erase(it);
		} else {
			++it;
		}
	}
}

void CKHRadioDlg::JoinWorkers(DWORD timeoutMs)
{
	const ULONGLONG deadline = GetTickCount64() + timeoutMs;
	int left = 0;
	for (auto& worker : m_workers) {
		if (!worker.thread.joinable()) {
			continue;
		}
		const ULONGLONG now = GetTickCount64();
		const DWORD wait = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
		if (WaitForSingleObject(worker.thread.native_handle(), wait) == WAIT_OBJECT_0) {
			worker.thread.join();
		} else {
			// Still blocked in a network call: let it finish on its own. It only
			// touches its own data and process-lifetime state.
			worker.thread.detach();
			left++;
		}
	}
	m_workers.clear();
	if (left) {
		KHLog::Format(L"exit: %d worker(s) still busy after %lu ms - left to finish by themselves", left, timeoutMs);
	}
}

void CKHRadioDlg::PopulateFilterLists()
{
	for (const auto& opt : s_khTypes) {
		const int idx = m_listType.AddString(opt.name);
		m_listType.SetItemData(idx, static_cast<DWORD_PTR>(opt.value));
	}

	// KHInsider lists next year's announced albums too.
	SYSTEMTIME now = {};
	GetLocalTime(&now);
	const int yearMax = static_cast<int>(now.wYear) + 1;

	int idx = m_listYear.AddString(L"Unknown");
	m_listYear.SetItemData(idx, 0);
	for (int year = yearMax; year >= KHRADIO_YEAR_MIN; year--) {
		CStringW str;
		str.Format(L"%d", year);
		idx = m_listYear.AddString(str);
		m_listYear.SetItemData(idx, static_cast<DWORD_PTR>(year));
	}

	for (const auto& opt : s_khPlatforms) {
		const int i = m_listPlatform.AddString(opt.name);
		m_listPlatform.SetItemData(i, static_cast<DWORD_PTR>(opt.value));
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
		for (int i = 0; i < l.GetCount(); i++) {
			if (l.GetSel(i) > 0) {
				n++;
			}
		}
		return n;
	};
	KHLog::Format(L"restore: types='%s'(%d sel) years='%s'(%d sel) platforms='%s'(%d sel)",
		s.types.GetString(), selCount(m_listType),
		s.years.GetString(), selCount(m_listYear),
		s.platforms.GetString(), selCount(m_listPlatform));
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

CStringW CKHRadioDlg::HistoryText(const CKHRadioHistory::AlbumEntry& album)
{
	CStringW str = album.title;
	if (str.IsEmpty()) {
		str = album.url;
	}
	str.AppendFormat(L"  (%u)", static_cast<unsigned>(album.tracks.size()));
	return str;
}

void CKHRadioDlg::RefreshHistoryList()
{
	if (!::IsWindow(m_listHistory.m_hWnd)) {
		return;
	}

	m_listHistory.SetRedraw(FALSE);
	m_listHistory.ResetContent();
	for (const auto& album : m_history.Albums()) {
		m_listHistory.AddString(HistoryText(album));
	}
	m_listHistory.SetRedraw(TRUE);
	m_listHistory.Invalidate();
}

// After RecordTrack moved an album to the top: move just that row, keeping the
// user's scroll position and selection on the same albums.
void CKHRadioDlg::HistoryRecorded(int previousIndex)
{
	if (!::IsWindow(m_listHistory.m_hWnd)) {
		return;
	}
	const auto& albums = m_history.Albums();
	const int count = m_listHistory.GetCount();
	const int expectedBefore = static_cast<int>(albums.size()) - (previousIndex < 0 ? 1 : 0);
	if (albums.empty() || count != expectedBefore || previousIndex >= count) {
		RefreshHistoryList();
		return;
	}
	const int top = m_listHistory.GetTopIndex();
	const int sel = m_listHistory.GetCurSel();
	m_listHistory.SetRedraw(FALSE);
	if (previousIndex >= 0) {
		m_listHistory.DeleteString(static_cast<UINT>(previousIndex));
	}
	m_listHistory.InsertString(0, HistoryText(albums.front()));
	if (sel != LB_ERR) {
		int newSel = sel;
		if (sel == previousIndex) {
			newSel = 0;
		} else if (previousIndex < 0 || sel < previousIndex) {
			newSel = sel + 1;
		}
		m_listHistory.SetCurSel(newSel);
	}
	if (top > 0 && (previousIndex < 0 || top <= previousIndex)) {
		m_listHistory.SetTopIndex(top + 1);
	} else {
		m_listHistory.SetTopIndex(top);
	}
	m_listHistory.SetRedraw(TRUE);
	m_listHistory.Invalidate(FALSE);
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

CStringW CKHRadioDlg::SessionStatusText(int status, LPARAM lParam) const
{
	CStringW retry;
	if (lParam > 0) {
		retry.Format(L" Retrying in %d s.", static_cast<int>(lParam));
	}
	switch (static_cast<KHWebSession::Status>(status)) {
		case KHWebSession::Status::Starting:
			if (lParam > 0) {
				CStringW text;
				text.Format(L"Reconnecting to KHInsider (attempt %d)...", static_cast<int>(lParam));
				return text;
			}
			return L"Checking KHInsider sign-in...";
		case KHWebSession::Status::Ready:
			return m_bRadioActive ? L"Connected to KHInsider again - the radio carries on."
				: L"Signed in. Pick your filters and roll an album.";
		case KHWebSession::Status::LoginRequired:
			return L"Sign-in needed: click 'Sign in to KHInsider...' above. The radio resumes by itself afterwards.";
		case KHWebSession::Status::Blocked:
			return L"KHInsider wants a browser check or is limiting requests." + retry
				+ L" To pass a check now, click 'Sign in to KHInsider...'.";
		case KHWebSession::Status::NetworkError:
			return L"Can't reach KHInsider (network or site down)." + retry;
		case KHWebSession::Status::Unavailable:
			return L"The built-in browser (WebView2) is not working." + retry
				+ L" If this keeps happening, install the Microsoft Edge WebView2 Runtime.";
		case KHWebSession::Status::LayoutChanged:
			return L"KHInsider's page format appears to have changed - KH Radio may need an update." + retry;
	}
	return CStringW();
}

LRESULT CKHRadioDlg::OnSessionStatus(WPARAM wParam, LPARAM lParam)
{
	const auto status = static_cast<KHWebSession::Status>(wParam);
	m_buttonLogin.SetWindowTextW(status == KHWebSession::Status::Ready ? L"KHInsider account..." : L"Sign in to KHInsider...");
	if (status != KHWebSession::Status::Ready) {
		// Queued, already-resolved tracks keep playing; the radio picks up again
		// by itself once the session is back.
		const bool firstCheck = status == KHWebSession::Status::Starting && lParam == 0;
		if (!firstCheck) {
			m_sessionNotice = true;
		}
		if (!firstCheck || !m_bRadioActive) {
			SetStatus(SessionStatusText(static_cast<int>(status), lParam));
		}
		return 0;
	}

	m_resolveRetryAt = 0;
	m_fetchRetryDelay = 0;
	if (m_pendingStart && !m_bFetching) {
		RunPendingFetch();
	} else if (!m_bFetching && (m_sessionNotice || !m_bRadioActive)) {
		SetStatus(SessionStatusText(static_cast<int>(status), lParam));
	}
	m_sessionNotice = false;
	KickResolver();
	MaybePrefetchNext();
	SetTimer(KHRADIO_AUTOSTART_TIMER, KHRADIO_AUTOSTART_DELAY, nullptr);
	return 0;
}

CSize CKHRadioDlg::GetNaturalSize() const
{
	return CSize(MulDiv(m_templateSize.cx, static_cast<int>(m_layoutDpi), static_cast<int>(m_templateDpi)),
		MulDiv(m_templateSize.cy, static_cast<int>(m_layoutDpi), static_cast<int>(m_templateDpi)));
}

// The history list may shrink to this height when the window is short.
static int MinHistoryHeight(UINT dpi)
{
	return MulDiv(64, static_cast<int>(dpi), 96);
}

int CKHRadioDlg::GetMinimumHeight() const
{
	const CSize natural = GetNaturalSize();
	for (const auto& item : m_layout) {
		if (::GetDlgCtrlID(item.window) == IDC_KHRADIO_HISTORY_LIST) {
			const int historyHeight = MulDiv(item.rect.Height(), static_cast<int>(m_layoutDpi), static_cast<int>(m_templateDpi));
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
	font.lfHeight = MulDiv(font.lfHeight, static_cast<int>(dpi), static_cast<int>(m_templateDpi));
	CFont next;
	if (next.CreateFontIndirectW(&font)) {
		SetFont(&next, FALSE);
		for (const auto& item : m_layout) { ::SendMessageW(item.window, WM_SETFONT, reinterpret_cast<WPARAM>(next.m_hObject), FALSE); }
		m_scaledFont.DeleteObject();
		m_scaledFont.Attach(next.Detach());
		const int rowHeight = ListItemHeight();
		for (CListBox* list : {&m_listType, &m_listYear, &m_listPlatform, &m_listHistory}) { list->SetItemHeight(0, static_cast<UINT>(rowHeight)); }
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

	StartFetch(true, m_history.Albums()[static_cast<size_t>(sel)].url);
}

void CKHRadioDlg::OnContextMenu(CWnd* pWnd, CPoint pt)
{
	if (pWnd != &m_listHistory) {
		__super::OnContextMenu(pWnd, pt);
		return;
	}
	int index = m_listHistory.GetCurSel();
	if (pt.x == -1 && pt.y == -1) {
		CRect r;
		if (index < 0 || m_listHistory.GetItemRect(index, r) == LB_ERR) {
			return;
		}
		pt = CPoint(r.left + r.Height(), r.bottom);
		m_listHistory.ClientToScreen(&pt);
	} else {
		CPoint client(pt);
		m_listHistory.ScreenToClient(&client);
		BOOL outside = TRUE;
		index = static_cast<int>(m_listHistory.ItemFromPoint(client, outside));
		if (outside) {
			return;
		}
		m_listHistory.SetCurSel(index);
	}
	if (index < 0 || index >= static_cast<int>(m_history.Albums().size())) {
		return;
	}
	const auto album = m_history.Albums()[static_cast<size_t>(index)];
	const CStringW text = KHShare::AlbumShareText(album.title, album.url);
	CMenu menu;
	menu.CreatePopupMenu();
	menu.AppendMenuW(MF_STRING | (text.IsEmpty() ? MF_GRAYED : 0), 1, L"Share this album");
	if (menu.TrackPopupMenu(TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, this) == 1) {
		SetStatus(KHShare::CopyToClipboard(m_hWnd, text) ? L"Album link copied \x2014 paste it to a friend"
			: L"Could not copy the link: another program is using the clipboard. Try again.");
	}
}

void CKHRadioDlg::PlaySharedLink(const KHShare::Link& link)
{
	if (link.kind == KHShare::LinkKind::None || link.albumUrl.IsEmpty()) {
		return;
	}
	KHLog::Format(L"share: playing a pasted %s link to %s", link.kind == KHShare::LinkKind::Track ? L"track" : L"album", link.albumUrl.GetString());
	m_shareNotice.Empty();
	StartFetch(true, link.albumUrl, false, true, link.kind == KHShare::LinkKind::Track ? link.trackKey : CStringW());
}

void CKHRadioDlg::CancelSharedLink()
{
	bool cancelled = false;
	if (m_bFetching && m_fetchShared) {
		(*m_pGen)++; // its result is dropped on arrival
		m_bFetching = false;
		cancelled = true;
	}
	if (m_pendingStart && m_pendingShared) {
		m_pendingStart = false;
		m_fetchRetryAt = 0;
		cancelled = true;
	}
	m_fetchShared = m_pendingShared = false;
	if (cancelled) {
		KHLog::Line(L"share: the pasted link was dropped - the user picked something else");
		SetStatus(L"Stopped loading the shared link.");
		MaybePrefetchNext();
	}
}

void CKHRadioDlg::StartFetch(bool bDirect, const CStringW& albumUrl, bool bAppend, bool shared, const CStringW& startAt)
{
	if (KHInsider::IsShuttingDown()) {
		return;
	}
	const auto session = KHWebSession::GetStatus();
	if (session != KHWebSession::Status::Ready) {
		// Remember the request; it runs once the session is back.
		m_pendingStart = true;
		m_pendingDirect = bDirect;
		m_pendingAppend = bAppend;
		m_pendingUrl = albumUrl;
		m_pendingShared = shared;
		m_pendingStartAt = startAt;
		m_fetchRetryAt = 0;
		if (!bAppend) {
			SetStatus(SessionStatusText(static_cast<int>(session), 0)
				+ (shared ? L"\r\nThe shared link plays as soon as KHInsider answers." : L"\r\nThe album loads as soon as KHInsider answers."));
		}
		return;
	}
	m_pendingStart = false;
	m_fetchRetryAt = 0;
	if (!bAppend && !shared) {
		m_shareNotice.Empty(); // the user moved on from a shared album
	}

	auto p = std::make_unique<FetchParams>();
	p->hWnd = m_hWnd;
	p->gen = ++(*m_pGen); // a running fetch is orphaned
	p->pGen = m_pGen;
	p->bDirect = bDirect;
	p->bAppend = bAppend;
	p->albumUrl = albumUrl;
	p->shared = shared;
	p->startAt = startAt;
	p->bAvoidPlayed = (m_checkAvoidPlayed.GetCheck() == BST_CHECKED);
	p->bFilterSpoken = (m_checkFilterSpoken.GetCheck() == BST_CHECKED);
	p->minTrackSeconds = KHRadioData::Get().minTrackSeconds;
	p->kept = KHRadioData::Kept();
	p->bExclusive = (m_checkExclusive.GetCheck() == BST_CHECKED);
	p->coverDir = KHRadioData::CoverDir();

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

	m_fetchDirect = bDirect;
	m_fetchAppend = bAppend;
	m_fetchUrl = albumUrl;
	m_fetchShared = shared;
	m_fetchStartAt = startAt;
	m_bFetching = true;
	m_fetchStarted = GetTickCount64();
	SetStatus(bAppend ? L"Queuing the next album..."
		: shared ? (startAt.IsEmpty() ? L"Loading the shared album..." : L"Loading the shared track...")
		: (bDirect ? L"Loading album..." : L"Rolling a random album..."));

	if (!StartWorker(L"fetch", [params = std::move(p)]() mutable { FetchThread(std::move(params)); })) {
		m_bFetching = false;
		SetStatus(L"Could not start the fetch thread.");
	}
}

unsigned CKHRadioDlg::ScheduleFetchRetry(KHInsider::FetchStatus why)
{
	m_pendingStart = true;
	m_pendingDirect = m_fetchDirect;
	m_pendingAppend = m_fetchAppend;
	m_pendingUrl = m_fetchUrl;
	m_pendingShared = m_fetchShared;
	m_pendingStartAt = m_fetchStartAt;
	if (why == KHInsider::FetchStatus::LoginRequired || why == KHInsider::FetchStatus::BrowserUnavailable) {
		m_fetchRetryAt = 0; // runs when the session reports Ready again
		KHLog::Line(L"radio: fetch waits for the KHInsider session to recover");
		return 0;
	}
	m_fetchRetryDelay = m_fetchRetryDelay ? std::min(m_fetchRetryDelay * 2, 300u) : 20u;
	m_fetchRetryAt = GetTickCount64() + m_fetchRetryDelay * 1000ull;
	KHLog::Format(L"radio: fetch retry in %u s", m_fetchRetryDelay);
	return m_fetchRetryDelay;
}

void CKHRadioDlg::RunPendingFetch()
{
	if (!m_pendingStart || m_bFetching) {
		return;
	}
	m_pendingStart = false;
	m_fetchRetryAt = 0;
	if (m_pendingAppend) {
		MaybePrefetchNext(); // only if the radio still needs another album
	} else {
		StartFetch(m_pendingDirect, m_pendingUrl, false, m_pendingShared, m_pendingStartAt);
	}
}

int CKHRadioDlg::CountQueuedAlbums() const
{
	if (!m_host) {
		return 0;
	}
	const auto& queue = m_host->Player().Queue();
	const size_t start = static_cast<size_t>(std::max(0, m_host->Player().CurrentIndex()));
	int albums = 0;
	CStringW last;
	for (size_t i = start; i < queue.size(); i++) {
		if (queue[i].IsSkipped()) {
			continue; // an album the filter emptied doesn't keep the radio going
		}
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
	// well before the current album ends. Only one fetch runs at a time, and a
	// scheduled retry waits for its time.
	if (!m_bRadioActive || m_bFetching) {
		return;
	}
	if (m_pendingStart && m_fetchRetryAt && GetTickCount64() < m_fetchRetryAt) {
		return;
	}
	const int queued = CountQueuedAlbums();
	const bool go = queued < KHRADIO_QUEUE_DEPTH;
	KHLog::Format(L"prefetch-check: queued=%d -> %s", queued, go ? L"FETCH" : L"skip");
	if (go) {
		StartFetch(false, L"", true);
	}
}

void CKHRadioDlg::StartCoverFetch(const CStringW& albumUrl)
{
	if (albumUrl.IsEmpty() || Contains(m_coverFetches, albumUrl)) {
		return; // nothing to do, or already on its way
	}
	auto p = std::make_unique<CoverParams>();
	p->hWnd = m_hWnd;
	p->albumUrl = albumUrl;
	p->coverPath = CoverFile(KHRadioData::CoverDir(), albumUrl);
	if (StartWorker(L"cover", [params = std::move(p)]() mutable { CoverThread(std::move(params)); })) {
		m_coverFetches.push_back(albumUrl);
	}
}

void CKHRadioDlg::CoverThread(std::unique_ptr<CoverParams> p)
{
	auto msg = std::make_unique<KHRadioCoverMsg>();
	msg->albumUrl = p->albumUrl;
	KHInsider::Album album;
	if (KHInsider::FetchAlbum(p->albumUrl, album, nullptr)) {
		album.url = p->albumUrl;
		msg->coverPath = DownloadCover(album, p->coverPath);
	}
	PostOwned(p->hWnd, WM_KHRADIO_COVER, std::move(msg));
}

void CKHRadioDlg::FetchThread(std::unique_ptr<FetchParams> p)
{
	const auto cancelled = [&]() {
		return (*p->pGen) != p->gen || KHInsider::IsShuttingDown();
	};
	const auto postText = [&](const CStringW& text) {
		auto m = std::make_unique<KHRadioTextMsg>();
		m->gen = p->gen;
		m->text = text;
		PostOwned(p->hWnd, WM_KHRADIO_STATUS, std::move(m));
	};

	// exclusive mode rerolls multi-platform albums, so it needs more tries to
	// land on a console-exclusive one
	const int attempts = p->bDirect ? 1 : (p->bExclusive ? 14 : 8);

	KHInsider::FetchStatus fetchStatus = KHInsider::FetchStatus::NetworkError;
	bool failed = false;
	int skippedShort = 0, skippedTitle = 0;
	std::vector<SkipReason> chosenSkips;
	KHInsider::Album chosen;
	bool have = false;

	for (int attempt = 1; attempt <= attempts && !cancelled(); attempt++) {
		KHInsider::Album album;
		const bool gotAlbum = p->bDirect
			? KHInsider::FetchAlbum(p->albumUrl, album, &fetchStatus)
			: KHInsider::FetchRandomAlbum(p->formBody, album, &fetchStatus);
		if (!gotAlbum) {
			failed = true; // network/session/parse failure; fetchStatus has the reason
			break;
		}
		const bool lastTry = attempt == attempts;

		// skip already-heard albums (random mode only)
		if (!p->bDirect && p->bAvoidPlayed && !lastTry && Contains(p->playedAlbums, album.url)) {
			CStringW status;
			status.Format(L"Already heard '%s' - rerolling...", album.title.GetString());
			postText(status);
			continue;
		}

		// exclusive-console mode (random only): skip albums tagged for any console
		// the user didn't pick, rerolling until we find a console-exclusive one
		if (!p->bDirect && p->bExclusive && !p->selectedPlatformNames.empty()
				&& !KHInsider::AlbumExclusiveTo(album.platforms, p->selectedPlatformNames)) {
			KHLog::Format(L"exclusive: '%s' [%s] is multi-platform%s", album.title.GetString(), album.platforms.GetString(),
				lastTry ? L" - keeping (no exclusive found)" : L" - rerolling");
			if (!lastTry) {
				CStringW status;
				status.Format(L"'%s' is multi-platform - rerolling for an exclusive...", album.title.GetString());
				postText(status);
				continue;
			}
			// last attempt: fall through and accept so the radio never dead-ends
		}

		// whole-album spoken filter: drama / voice albums are entirely spoken,
		// so skip the album by title
		CStringW albumCue;
		if (!p->bDirect && p->bFilterSpoken && KHInsider::IsSpokenTitle(album.title, &albumCue) && !lastTry) {
			KHLog::Format(L"filter: album '%s' says '%s' -> a drama / voice album, rerolling", album.title.GetString(), albumCue.GetString());
			CStringW status;
			status.Format(L"'%s' is a drama/voice album - rerolling...", album.title.GetString());
			postText(status);
			continue;
		}

		// cheap per-track checks from the album page itself: very short tracks and
		// titles that say drama / voice track. They stay queued as skipped rows; the
		// audio check runs later, a few tracks ahead of playback.
		std::vector<SkipReason> skips(album.tracks.size(), SkipReason::None);
		int nShort = 0, nTitle = 0, playable = 0;
		for (size_t t = 0; t < album.tracks.size(); t++) {
			const auto& track = album.tracks[t];
			const bool exempt = !p->bFilterSpoken
				|| (!p->startAt.IsEmpty() && KHShare::TrackKeyOf(track.pageUrl) == p->startAt) // the shared track plays whatever the filter says
				|| p->kept.count(KHRadioData::KeepKey(track.pageUrl)) > 0;
			CStringW cue;
			if (exempt) {
			} else if (KHInsider::IsShortTrack(track.durationSec, p->minTrackSeconds)) {
				skips[t] = SkipReason::Short;
				nShort++;
				KHLog::Format(L"filter: '%s' is %d s (under %d s) -> SKIP (short)", track.name.GetString(), track.durationSec, p->minTrackSeconds);
			} else if (KHInsider::SpokenTitleCue(track.name, &cue) == KHInsider::TitleCue::Strong) {
				skips[t] = SkipReason::Title;
				nTitle++;
				KHLog::Format(L"filter: '%s' title says '%s' -> SKIP (title)", track.name.GetString(), cue.GetString());
			}
			playable += skips[t] == SkipReason::None ? 1 : 0;
		}
		skippedShort = nShort;
		skippedTitle = nTitle;

		if (playable == 0) {
			if (!p->bDirect && !lastTry) {
				postText(L"Only short or drama / voice tracks here - rerolling...");
				continue;
			}
			break;
		}
		chosen = std::move(album);
		chosenSkips = std::move(skips);
		have = true;
		break;
	}

	if (cancelled()) {
		return;
	}

	auto done = std::make_unique<KHRadioDoneMsg>();
	done->gen = p->gen;
	done->bAppend = p->bAppend;
	done->failed = failed;
	done->status = fetchStatus;
	done->skippedShort = skippedShort;
	done->skippedTitle = skippedTitle;
	if (failed) {
		switch (fetchStatus) {
			case KHInsider::FetchStatus::LoginRequired:
				done->text = L"Sign-in needed: click 'Sign in to KHInsider...' above. The radio resumes by itself afterwards.";
				break;
			case KHInsider::FetchStatus::BrowserUnavailable:
				done->text = L"The built-in browser is not ready.";
				break;
			case KHInsider::FetchStatus::Blocked:
				done->text = L"KHInsider refused the request (browser check or rate limit).";
				break;
			case KHInsider::FetchStatus::LayoutChanged:
				done->text = L"Couldn't read the album page - KHInsider's page format appears to have changed.";
				break;
			default:
				done->text = L"Can't reach KHInsider - check your connection.";
				break;
		}
	} else if (!have) {
		done->text = skippedShort + skippedTitle > 0
			? CStringW(L"Every track was too short or a drama / voice track - try unchecking 'Filter spoken / short / silent tracks'.")
			: CStringW(L"No playable tracks found - try different filters.");
	}

	CStringW coverPath;
	if (have) {
		coverPath = CoverFile(p->coverDir, chosen.url);
		auto am = std::make_unique<KHRadioAlbumMsg>();
		am->gen = p->gen;
		am->bAppend = p->bAppend;
		am->shared = p->shared;
		am->startAt = p->startAt;
		am->album = chosen;
		am->skips = chosenSkips;
		am->coverPath = FileHasData(coverPath) ? coverPath : CStringW();
		done->queued = static_cast<int>(chosen.tracks.size()) - skippedShort - skippedTitle;
		done->title = chosen.title;
		const bool needCover = am->coverPath.IsEmpty();
		PostOwned(p->hWnd, WM_KHRADIO_ALBUM, std::move(am));
		PostOwned(p->hWnd, WM_KHRADIO_DONE, std::move(done));
		if (needCover) {
			// the album is queued already; the cover follows when it arrives
			auto cm = std::make_unique<KHRadioCoverMsg>();
			cm->albumUrl = chosen.url;
			cm->coverPath = DownloadCover(chosen, coverPath);
			PostOwned(p->hWnd, WM_KHRADIO_COVER, std::move(cm));
		}
		return;
	}
	PostOwned(p->hWnd, WM_KHRADIO_DONE, std::move(done));
}

void CKHRadioDlg::KickResolver()
{
	if (m_bResolving || !m_host || KHInsider::IsShuttingDown()
			|| KHWebSession::GetStatus() != KHWebSession::Status::Ready
			|| (m_resolveRetryAt && GetTickCount64() < m_resolveRetryAt)) {
		return;
	}
	const auto& queue = m_host->Player().Queue();
	const int start = std::max(0, m_host->Player().CurrentIndex());
	// the playing track and the next few playable ones; skipped rows don't use up the window
	const int last = AheadLimit(queue, start, KHRADIO_RESOLVE_AHEAD);
	auto p = std::make_unique<ResolveParams>();
	for (int i = start; i <= last && p->items.size() < KHRADIO_RESOLVE_BATCH; i++) {
		const QueueItem& item = queue[static_cast<size_t>(i)];
		if (!item.IsSkipped() && !item.IsResolved() && !item.pageUrl.IsEmpty()) {
			p->items.push_back({ item.id, item.pageUrl, item.ref.name, item.shared || item.keep || item.judged });
		}
	}
	if (p->items.empty()) {
		return;
	}
	p->hWnd = m_hWnd;
	p->gen = ++(*m_resolveGen);
	p->pGen = m_resolveGen;
	p->bFilterSpoken = (m_checkFilterSpoken.GetCheck() == BST_CHECKED);
	m_bResolving = true;
	m_resolveStarted = GetTickCount64();
	if (!StartWorker(L"resolve", [params = std::move(p)]() mutable { ResolveThread(std::move(params)); })) {
		m_bResolving = false;
	}
}

void CKHRadioDlg::ResolveThread(std::unique_ptr<ResolveParams> p)
{
	const auto cancelled = [&]() {
		return (*p->pGen) != p->gen || KHInsider::IsShuttingDown();
	};
	bool retryLater = false;
	for (size_t i = 0; i < p->items.size() && !cancelled(); i++) {
		if (i > 0) {
			Sleep(KHRADIO_RESOLVE_PACE_MS); // be gentle with the site
			if (cancelled()) {
				return;
			}
		}
		const auto& item = p->items[i];
		auto msg = std::make_unique<KHRadioResolvedMsg>();
		msg->gen = p->gen;
		msg->id = item.id;
		msg->name = item.name;
		KHInsider::FetchStatus status = KHInsider::FetchStatus::NetworkError;
		const CStringW url = KHInsider::ResolveTrackAudioUrl(item.pageUrl, &status);
		if (!url.IsEmpty()) {
			if (p->bFilterSpoken && !item.keep) {
				msg->decision = KHInsider::ClassifyTrackAudio(url); // two 256 KB windows; not judged for non-mp3
			}
			msg->outcome = ResolveOutcome::Resolved;
			msg->audioUrl = url;
		} else {
			switch (status) {
				case KHInsider::FetchStatus::LoginRequired:
				case KHInsider::FetchStatus::Blocked:
				case KHInsider::FetchStatus::BrowserUnavailable:
					msg->outcome = ResolveOutcome::SessionProblem;
					break;
				case KHInsider::FetchStatus::LayoutChanged:
					msg->outcome = ResolveOutcome::NotFound;
					break;
				default:
					msg->outcome = ResolveOutcome::Failed;
					break;
			}
		}
		const ResolveOutcome outcome = msg->outcome;
		if (cancelled()) {
			return;
		}
		PostOwned(p->hWnd, WM_KHRADIO_RESOLVED, std::move(msg));
		if (outcome == ResolveOutcome::SessionProblem) {
			retryLater = true;
			break; // the session decides what happens next
		}
		if (outcome == ResolveOutcome::Failed) {
			retryLater = true;
		}
	}
	if (cancelled()) {
		return;
	}
	auto done = std::make_unique<KHRadioResolveDoneMsg>();
	done->gen = p->gen;
	done->retryLater = retryLater;
	PostOwned(p->hWnd, WM_KHRADIO_RESOLVE_DONE, std::move(done));
}

LRESULT CKHRadioDlg::OnKHRadioResolved(WPARAM, LPARAM lParam)
{
	auto m = TakeOwned<KHRadioResolvedMsg>(lParam);
	if (!m || m->gen != (*m_resolveGen) || !m_host) {
		return 0;
	}
	CRadioPlayer& player = m_host->Player();
	const int index = player.FindById(m->id);
	if (index < 0) {
		return 0; // the item left the queue meanwhile
	}
	switch (m->outcome) {
		case ResolveOutcome::Resolved:
			// a skipped track keeps its address, so "Play anyway" can start it at once
			if (m->decision.judged && ApplyAudioCheck(index, m->decision, L"mp3")) {
				player.Resolve(player.FindById(m->id), m->audioUrl);
				KickResolver();
				break;
			}
			player.Resolve(index, m->audioUrl);
			KickPreparer(); // an Ogg track ahead can be decoded now
			break;
		case ResolveOutcome::NotFound:
			player.Remove(index);
			SetStatus(L"Skipped '" + m->name + L"': its page has no audio link (KHInsider's page format may have changed).");
			break;
		case ResolveOutcome::Failed:
			// Only failures while the session is fine count against the track.
			if (KHWebSession::GetStatus() == KHWebSession::Status::Ready) {
				QueueItem* item = player.Item(index);
				if (item && ++item->resolveFailures >= KHRADIO_RESOLVE_MAX_FAILURES) {
					KHLog::Format(L"resolve: giving up on '%s' after %d failures", m->name.GetString(), item->resolveFailures);
					player.Remove(index);
					SetStatus(L"Skipped '" + m->name + L"': its page would not load.");
				}
			}
			break;
		case ResolveOutcome::SessionProblem:
			break; // stays queued; the session status explains and recovers
	}
	return 0;
}

LRESULT CKHRadioDlg::OnKHRadioResolveDone(WPARAM, LPARAM lParam)
{
	auto m = TakeOwned<KHRadioResolveDoneMsg>(lParam);
	if (!m || m->gen != (*m_resolveGen)) {
		return 0;
	}
	m_bResolving = false;
	if (m->retryLater) {
		m_resolveRetryAt = GetTickCount64() + KHRADIO_RESOLVE_RETRY_MS;
	} else {
		KickResolver(); // the next few, if the playing track moved on
	}
	MaybePrefetchNext(); // dropped tracks may have emptied an album
	return 0;
}

void CKHRadioDlg::OnNeedResolve()
{
	KickResolver();
	KickPreparer();
}

void CKHRadioDlg::ShowPreparing(const QueueItem& item)
{
	SetStatus(L"Preparing '" + item.ref.name + (item.prepare == QueueItem::Prepare::DecodeOgg
		? L"' (Ogg Vorbis: downloading and decoding) - it plays as soon as it is ready."
		: L"' (downloading the m4a file) - it plays as soon as it is ready."));
}

void CKHRadioDlg::KickPreparer()
{
	if (!m_host || KHInsider::IsShuttingDown()) {
		return;
	}
	CRadioPlayer& player = m_host->Player();
	const auto& queue = player.Queue();
	if (queue.empty()) {
		return;
	}
	const int cur = std::max(0, player.CurrentIndex());
	const int last = AheadLimit(queue, cur, KHRADIO_RESOLVE_AHEAD);
	int target = -1;
	bool inFlightNeeded = false;
	for (int i = cur; i <= last; i++) {
		const QueueItem& item = queue[static_cast<size_t>(i)];
		if (item.IsSkipped()) {
			continue;
		}
		if (m_bPreparing && item.id == m_prepareId) {
			inFlightNeeded = true;
		}
		if (target < 0 && item.IsResolved() && item.NeedsPrepare()) {
			target = i;
		}
	}
	const bool currentWaits = target == cur && player.IsWaitingForResolve();
	if (m_bPreparing) {
		if (inFlightNeeded && (!currentWaits || queue[static_cast<size_t>(cur)].id == m_prepareId)) {
			if (currentWaits) {
				ShowPreparing(queue[static_cast<size_t>(cur)]);
			}
			return; // the one running is still the one needed first
		}
		KHLog::Line(L"prepare: the track being prepared is not needed first any more - cancelled");
		(*m_prepareGen)++;
		m_bPreparing = false;
		m_prepareId = 0;
	}
	if (target < 0 || (!currentWaits && m_prepareRetryAt && GetTickCount64() < m_prepareRetryAt)) {
		return;
	}
	const QueueItem& item = queue[static_cast<size_t>(target)];
	auto p = std::make_unique<PrepareParams>();
	p->hWnd = m_hWnd;
	p->gen = ++(*m_prepareGen);
	p->pGen = m_prepareGen;
	p->id = item.id;
	p->url = item.audioUrl;
	p->name = item.ref.name;
	p->mode = item.prepare;
	p->current = target == cur;
	p->bFilterSpoken = (m_checkFilterSpoken.GetCheck() == BST_CHECKED) && !item.shared && !item.keep && !item.judged;
	m_bPreparing = true;
	m_prepareId = item.id;
	m_prepareStarted = GetTickCount64();
	KHLog::Format(L"prepare: '%s' (%s)%s", item.ref.name.GetString(),
		item.prepare == QueueItem::Prepare::DecodeOgg ? L"decode Ogg to WAV" : L"download m4a", p->current ? L" - the player waits for it" : L"");
	if (currentWaits) {
		ShowPreparing(item);
	}
	if (!StartWorker(L"prepare", [params = std::move(p)]() mutable { PrepareThread(std::move(params)); })) {
		m_bPreparing = false;
		m_prepareId = 0;
	}
}

void CKHRadioDlg::PrepareThread(std::unique_ptr<PrepareParams> p)
{
	const std::function<bool()> cancelled = [gen = p->gen, pGen = p->pGen]() {
		return (*pGen) != gen || KHInsider::IsShuttingDown();
	};
	auto msg = std::make_unique<KHRadioPreparedMsg>();
	msg->gen = p->gen;
	msg->id = p->id;
	msg->name = p->name;
	const auto post = [&](PrepareOutcome outcome, const CStringW& path) {
		if (cancelled()) {
			if (!path.IsEmpty()) {
				DeleteFileW(path);
			}
			return;
		}
		msg->outcome = outcome;
		msg->path = path;
		PostOwned(p->hWnd, WM_KHRADIO_PREPARED, std::move(msg));
	};

	const bool ogg = p->mode == QueueItem::Prepare::DecodeOgg;
	const CStringW final = KHAudioCache::PathFor(p->id, ogg ? L"wav" : L"m4a");
	CStringW partExt;
	partExt.Format(L"%u.%s.part", p->gen, ogg ? L"ogg" : L"m4a");
	const CStringW download = KHAudioCache::PathFor(p->id, partExt);
	if (final.IsEmpty()) {
		post(ogg ? PrepareOutcome::Stream : PrepareOutcome::Failed, CStringW()); // the cache is off
		return;
	}
	if (!p->current && KHAudioCache::TotalBytes() >= KHRADIO_CACHE_CAP) {
		post(PrepareOutcome::NoRoom, CStringW());
		return;
	}
	if (!KHInsider::DownloadAudioToFile(p->url, download, KHRADIO_MAX_AUDIO_DOWNLOAD, cancelled)) {
		post(PrepareOutcome::Failed, CStringW());
		return;
	}
	if (!ogg) {
		if (MoveFileExW(download, final, MOVEFILE_REPLACE_EXISTING)) {
			post(PrepareOutcome::Ready, final);
		} else {
			DeleteFileW(download);
			post(PrepareOutcome::Failed, CStringW());
		}
		return;
	}

	// an upcoming track may only use what the cap leaves; the one the player
	// waits for may push older files out
	unsigned long long room = KHRADIO_MAX_WAV_BYTES;
	if (!p->current) {
		const unsigned long long used = KHAudioCache::TotalBytes();
		room = used < KHRADIO_CACHE_CAP ? std::min(room, KHRADIO_CACHE_CAP - used) : 0;
	}
	KHAudioCache::DecodeInfo info;
	const auto result = KHAudioCache::OggToWav(download, final, room, cancelled, &info);
	DeleteFileW(download);
	switch (result) {
		case KHAudioCache::DecodeResult::Ok:
			if (p->bFilterSpoken) {
				msg->decision = KHInsider::ClassifyMonoPcm(info.monoPrefix, info.monoMiddle, info.sampleRate);
			}
			post(PrepareOutcome::Ready, final);
			return;
		case KHAudioCache::DecodeResult::Cancelled:
			return;
		case KHAudioCache::DecodeResult::TooBig:
			KHLog::Format(L"prepare: '%s' is too long for the cache%s", p->name.GetString(), p->current ? L" - streaming it instead" : L" for now");
			post(p->current ? PrepareOutcome::Stream : PrepareOutcome::NoRoom, CStringW());
			return;
		case KHAudioCache::DecodeResult::NotVorbis:
		case KHAudioCache::DecodeResult::Failed:
			post(PrepareOutcome::Stream, CStringW());
			return;
	}
}

LRESULT CKHRadioDlg::OnKHRadioPrepared(WPARAM, LPARAM lParam)
{
	auto m = TakeOwned<KHRadioPreparedMsg>(lParam);
	if (!m) {
		return 0;
	}
	if (m->gen != (*m_prepareGen) || !m_host) {
		if (!m->path.IsEmpty() && KHAudioCache::IsCacheFile(m->path)) {
			DeleteFileW(m->path); // from a run that was cancelled meanwhile
		}
		return 0;
	}
	m_bPreparing = false;
	m_prepareId = 0;
	CRadioPlayer& player = m_host->Player();
	const int index = player.FindById(m->id);
	if (index < 0) {
		if (!m->path.IsEmpty()) {
			DeleteFileW(m->path); // the item left the queue meanwhile
		}
		KickPreparer();
		return 0;
	}
	switch (m->outcome) {
		case PrepareOutcome::Ready:
			if (m->decision.judged && ApplyAudioCheck(index, m->decision, L"ogg")) {
				DeleteFileW(m->path); // prepared again if the user plays it anyway
				break;
			}
			player.Prepared(index, m->path);
			break;
		case PrepareOutcome::Stream:
			KHLog::Format(L"prepare: '%s' could not be decoded here - trying to stream it as before", m->name.GetString());
			player.Prepared(index, CStringW());
			break;
		case PrepareOutcome::NoRoom:
			m_prepareRetryAt = GetTickCount64() + KHRADIO_PREPARE_RETRY_MS;
			break;
		case PrepareOutcome::Failed: {
			QueueItem* item = player.Item(index);
			if (item && ++item->prepareFailures >= KHRADIO_PREPARE_MAX_FAILURES) {
				if (item->prepare == QueueItem::Prepare::DecodeOgg) {
					KHLog::Format(L"prepare: '%s' would not download - trying to stream it", m->name.GetString());
					player.Prepared(index, CStringW());
				} else {
					KHLog::Format(L"prepare: giving up on '%s' after %d failed downloads", m->name.GetString(), item->prepareFailures);
					player.Remove(index);
					SetStatus(L"Skipped '" + m->name + L"': it would not download.");
				}
			} else {
				m_prepareRetryAt = GetTickCount64() + KHRADIO_PREPARE_RETRY_MS;
			}
			break;
		}
	}
	PruneCache();
	KickPreparer();
	return 0;
}

void CKHRadioDlg::PruneCache()
{
	if (!m_host || KHAudioCache::Dir().IsEmpty()) {
		return;
	}
	CRadioPlayer& player = m_host->Player();
	const auto& queue = player.Queue();
	const int cur = player.CurrentIndex();
	std::vector<unsigned long long> keep;
	unsigned long long protect = 0;
	if (cur >= 0 && cur < static_cast<int>(queue.size())) {
		const int last = AheadLimit(queue, cur, KHRADIO_RESOLVE_AHEAD);
		for (int i = cur; i <= last; i++) {
			keep.push_back(queue[static_cast<size_t>(i)].id);
		}
		protect = queue[static_cast<size_t>(cur)].id;
	}
	if (m_bPreparing) {
		keep.push_back(m_prepareId);
	}
	KHAudioCache::Prune(keep, protect, KHRADIO_CACHE_CAP);
	for (int i = 0; i < static_cast<int>(queue.size()); i++) {
		QueueItem* item = player.Item(i);
		if (item && !item->localPath.IsEmpty() && GetFileAttributesW(item->localPath) == INVALID_FILE_ATTRIBUTES) {
			item->localPath.Empty(); // let go: prepared again if it is played again
		}
	}
}

bool CKHRadioDlg::ApplyAudioCheck(int index, const KHAudio::Decision& d, LPCWSTR route)
{
	CRadioPlayer& player = m_host->Player();
	QueueItem* item = player.Item(index);
	if (!item) {
		return false;
	}
	item->judged = true;
	CStringW album = item->ref.albumUrl;
	album.MakeLower();
	if (m_albumTally.size() > 64 && m_albumTally.find(album) == m_albumTally.end()) {
		m_albumTally.clear(); // only the albums in the queue matter
	}
	KHAudio::AlbumTally& tally = m_albumTally[album];
	CStringW cueText;
	const KHInsider::TitleCue cue = KHInsider::SpokenTitleCue(item->ref.name, &cueText);
	const KHAudio::FilterCall call = KHAudio::ApplyAlbumContext(d, cue, tally);
	const CStringW name = item->ref.name;
	KHLog::Format(L"filter (%s): '%s' %s ; title cue: %s ; album so far %d/%d talking -> %s (%s)", route, name.GetString(),
		KHAudio::Describe(d).GetString(), cue == KHInsider::TitleCue::None ? L"none" : cueText.GetString(),
		tally.talking, tally.judged, call.skip == SkipReason::None ? L"keep" : L"SKIP", call.rule);
	tally.Add(d);
	if (call.skip == SkipReason::None) {
		return false;
	}
	player.MarkSkipped(index, call.skip);
	SetStatus(L"Skipped '" + name + L"': " + SkipReasonText(call.skip) + L".\r\nRight-click it in the queue to play it anyway.");
	return true;
}

LRESULT CKHRadioDlg::OnKHRadioStatus(WPARAM, LPARAM lParam)
{
	auto m = TakeOwned<KHRadioTextMsg>(lParam);
	if (m && m->gen == (*m_pGen)) {
		SetStatus(m->text);
	}
	return 0;
}

LRESULT CKHRadioDlg::OnKHRadioAlbum(WPARAM, LPARAM lParam)
{
	auto m = TakeOwned<KHRadioAlbumMsg>(lParam);
	if (!m || m->gen != (*m_pGen) || !m_host) {
		return 0;
	}
	const KHInsider::Album& album = m->album;
	m_lastAlbumTitle = album.title;

	CRadioPlayer& player = m_host->Player();
	if (!m->bAppend) {
		player.Clear(); // the user rolled a new album: switch now
	}
	const bool ranDry = !player.IsActive();
	// a shared track link starts at that track; the rest of the album follows
	int startTrack = 0;
	if (!m->bAppend && m->shared && !m->startAt.IsEmpty()) {
		startTrack = KHShare::FindTrack(album.tracks, m->startAt);
		if (startTrack < 0) {
			KHLog::Format(L"share: the shared track '%s' is not on %s - playing the album from the start", m->startAt.GetString(), album.url.GetString());
			m_shareNotice = L"The shared track is not on the album page any more, so the album plays from the start.";
			startTrack = 0;
		}
	}
	int first = -1, playable = 0;
	for (size_t t = 0; t < album.tracks.size(); t++) {
		const auto& track = album.tracks[t];
		QueueItem item;
		item.id = NewQueueItemId();
		item.pageUrl = track.pageUrl;
		item.skip = t < m->skips.size() ? m->skips[t] : SkipReason::None;
		item.keep = KHRadioData::IsKept(track.pageUrl);
		playable += item.IsSkipped() ? 0 : 1;
		item.label.Format(L"%02d. %s", track.number, track.name.GetString());
		item.ref = { track.name, album.url, album.title, m->coverPath, album.platforms };
		item.shared = m->shared && !m->startAt.IsEmpty() && m_shareNotice.IsEmpty() && static_cast<int>(t) == startTrack;
		const int at = player.Append(item);
		if (static_cast<int>(t) == startTrack) {
			first = at;
		}
	}
	// Jump-start only for a new album or when playback genuinely ran dry; a
	// paused or loading player keeps what it is doing. A skipped first track
	// starts at the next playable one.
	if (first >= 0 && (!m->bAppend || ranDry)) {
		player.PlayIndex(first);
	}
	if (m->coverPath.IsEmpty() && !Contains(m_coverFetches, album.url)) {
		m_coverFetches.push_back(album.url); // the fetch worker downloads it next
	}

	CStringW status;
	status.Format(m->bAppend ? L"Up next: %s (%d tracks)" : L"Found: %s (%d tracks) - starting...",
		album.title.GetString(), playable);
	if (!album.platforms.IsEmpty()) {
		status.AppendFormat(L"\r\nPlatforms: %s", album.platforms.GetString());
	}
	SetStatus(status);
	KickResolver();
	return 0;
}

LRESULT CKHRadioDlg::OnKHRadioDone(WPARAM, LPARAM lParam)
{
	auto m = TakeOwned<KHRadioDoneMsg>(lParam);
	if (!m || m->gen != (*m_pGen)) {
		return 0;
	}

	m_bFetching = false;

	if (m->queued > 0) {
		m_fetchRetryDelay = 0;
		if (!m->bAppend) {
			m_bRadioActive = true; // a replace fetch starts a radio session
		}
		KHLog::Format(L"done: append=%d queued=%d tracks -> albums queued=%d", m->bAppend ? 1 : 0, m->queued, CountQueuedAlbums());
		CStringW status;
		status.Format(m->bAppend ? L"Queued next: %s" : L"Playing: %s", m->title.GetString());
		if (m->skippedShort + m->skippedTitle > 0) {
			status.AppendFormat(L"  (%d skipped:", m->skippedShort + m->skippedTitle);
			if (m->skippedShort) {
				status.AppendFormat(L" %d too short", m->skippedShort);
			}
			if (m->skippedTitle) {
				status.AppendFormat(L"%s %d titled drama / voice", m->skippedShort ? L"," : L"", m->skippedTitle);
			}
			status += L")";
		}
		if (!m->bAppend && !m_shareNotice.IsEmpty()) {
			status += L"\r\n" + m_shareNotice;
		}
		SetStatus(status);
		MaybePrefetchNext(); // top the queue up if still short
		return 0;
	}

	KHLog::Format(L"done: fetch produced no tracks (failed=%d, status=%d)", m->failed ? 1 : 0, static_cast<int>(m->status));
	CStringW text = m->text;
	// A running radio keeps trying by itself; a user's request is retried only
	// when the failure was the site or the network, not the filters.
	if (m->failed && (m_bRadioActive || !m->bAppend)) {
		const unsigned delay = ScheduleFetchRetry(m->status);
		if (delay) {
			text.AppendFormat(L" Retrying in %u s.", delay);
		}
	} else if (!m->failed && m->bAppend && m_bRadioActive) {
		const unsigned delay = ScheduleFetchRetry(KHInsider::FetchStatus::NetworkError);
		text.AppendFormat(L" Trying again in %u s.", delay);
	}
	SetStatus(text);
	return 0;
}

LRESULT CKHRadioDlg::OnKHRadioCover(WPARAM, LPARAM lParam)
{
	auto m = TakeOwned<KHRadioCoverMsg>(lParam);
	if (!m) {
		return 0;
	}
	m_coverFetches.erase(std::remove_if(m_coverFetches.begin(), m_coverFetches.end(),
		[&](const CStringW& u) { return u.CompareNoCase(m->albumUrl) == 0; }), m_coverFetches.end());
	if (m_host && !m->coverPath.IsEmpty()) {
		// the cover may arrive after the track started, so show it now instead of
		// waiting for the next album
		m_host->Player().UpdateAlbumCover(m->albumUrl, m->coverPath);
		m_host->ShowAlbumCover(m->albumUrl, m->coverPath);
		KHLog::Format(L"cover ready: %s", m->coverPath.GetString());
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
		KHLog::Line(resumed ? L"radio: adopted restored queue on startup -> active" : L"radio: playback started -> active");
	}

	if (!resumed) {
		HistoryRecorded(m_history.RecordTrack(track.albumUrl, track.albumTitle, track.name));
	}

	CStringW status;
	status.Format(L"Now playing: %s - %s", track.albumTitle.GetString(), track.name.GetString());
	if (!m_shareNotice.IsEmpty()) {
		status += L"\r\n" + m_shareNotice; // once, with the first track of the shared album
		m_shareNotice.Empty();
	} else if (!track.platforms.IsEmpty()) {
		status.AppendFormat(L"\r\nPlatforms: %s", track.platforms.GetString());
	}
	SetStatus(status);
	m_sessionNotice = false;

	if (track.albumUrl != m_currentPlayingAlbum) {
		// a new album just started playing: switch the cover
		m_currentPlayingAlbum = track.albumUrl;
		if (FileHasData(track.coverPath)) {
			m_host->ShowAlbumCover(track.albumUrl, track.coverPath);
		} else {
			m_host->ShowAlbumCover(track.albumUrl, L"");
			if (!Contains(m_coverFetches, track.albumUrl)) {
				KHLog::Format(L"radio: fetching cover for %s", track.albumUrl.GetString());
				StartCoverFetch(track.albumUrl);
			}
		}
		KHLog::Format(L"radio: now playing album '%s', queued=%d", track.albumTitle.GetString(), CountQueuedAlbums());
	}

	// Covers a manual jump to the end of the queue too: the next album gets
	// fetched before this track ends.
	MaybePrefetchNext();
	KickResolver();
	m_prepareRetryAt = 0; // a track moved on: the cache may have room again
	PruneCache();
	KickPreparer();
}


bool CKHRadioDlg::ContinueRadioAtEnd()
{
	if (!m_bRadioActive) {
		return false; // not a radio session - let the player stop normally
	}

	// The queue ran dry (the prefetched album wasn't ready in time, or a fetch
	// failed). Make sure a fetch is running or scheduled; its first track
	// jump-starts playback on arrival.
	KHLog::Line(L"radio: queue reached end while active - continuing");
	MaybePrefetchNext();
	return true;
}
