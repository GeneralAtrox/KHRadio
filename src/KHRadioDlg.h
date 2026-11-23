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

#pragma once

#include <afxwin.h>
#include <atomic>
#include <map>
#include <memory>
#include <thread>
#include <vector>
#include "History.h"
#include "AudioClassify.h"
#include "KHInsider.h"
#include "RadioTypes.h"
#include "ShareLink.h"

class CRadioPlayer;

#define WM_KHRADIO_STATUS       (WM_APP + 850)
#define WM_KHRADIO_ALBUM        (WM_APP + 851)
#define WM_KHRADIO_RESOLVED     (WM_APP + 852)
#define WM_KHRADIO_DONE         (WM_APP + 853)
#define WM_KHRADIO_COVER        (WM_APP + 854)
#define WM_KHRADIO_RESOLVE_DONE (WM_APP + 856)
#define WM_KHRADIO_PREPARED     (WM_APP + 857)

// What the dialog needs from the main window.
struct IKHRadioHost {
	virtual CRadioPlayer& Player() = 0;
	// Show this album's cover if the album is the one playing (empty path = placeholder).
	virtual void ShowAlbumCover(const CStringW& albumUrl, const CStringW& coverPath) = 0;
};

class CKHRadioDlg : public CDialog
{
public:
	CKHRadioDlg();
	~CKHRadioDlg() override;

	BOOL Create(CWnd* pParent, IKHRadioHost* host);

	enum { IDD = IDD_KHRADIO_DLG };

	CListBox m_listType;
	CListBox m_listYear;
	CListBox m_listPlatform;
	CButton  m_buttonClear;
	CButton  m_checkAvoidPlayed;
	CButton  m_checkFilterSpoken;
	CButton  m_checkExclusive;
	CButton  m_buttonRandom;
	CButton  m_buttonLogin;
	CStatic  m_staticStatus;
	CListBox m_listHistory;

	// called by the main window when a queue item starts playing
	void OnPlaybackStarted(const QueueItem& item, bool resumed);
	// the player wants to play an item whose audio URL is not known yet
	void OnNeedResolve();

	// called when the queue runs out; returns true if the radio is taking over
	// (fetching the next album) instead of stopping.
	bool ContinueRadioAtEnd();
	void ScaleForDPI(UINT dpi);
	CSize GetNaturalSize() const;
	int GetMinimumHeight() const;
	void ApplyTheme();
	void SetStatus(const CStringW& text);
	// A link pasted by the user: load the album the way a history replay does,
	// starting at the shared track if there is one. Waits for sign-in like any
	// other request; anything else the user starts replaces it.
	void PlaySharedLink(const KHShare::Link& link);
	// The user did something else (picked a queue row, skipped): drop a shared
	// link that is still loading or waiting for the session.
	void CancelSharedLink();

protected:
	void DoDataExchange(CDataExchange* pDX) override;
	BOOL OnInitDialog() override;
	void OnOK() override {}      // child dialog: Enter/Esc must not destroy it
	void OnCancel() override {}
	DECLARE_MESSAGE_MAP()

	afx_msg void OnDestroy();
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);
	afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct);
	afx_msg void OnMeasureItem(int nIDCtl, LPMEASUREITEMSTRUCT lpMeasureItemStruct);
	int ListItemHeight();
	afx_msg void OnSelChangeFilters();
	afx_msg void OnClearFilters();
	afx_msg void OnAvoidPlayedClicked();
	afx_msg void OnFilterSpokenClicked();
	afx_msg void OnExclusiveClicked();
	afx_msg void OnRandomAlbum();
	afx_msg void OnLogin();
	afx_msg void OnSize(UINT type, int cx, int cy);
	afx_msg LRESULT OnSessionStatus(WPARAM wParam, LPARAM lParam);
	afx_msg void OnHistoryDblClk();
	afx_msg void OnContextMenu(CWnd* pWnd, CPoint pt);
	afx_msg LRESULT OnKHRadioStatus(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnKHRadioAlbum(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnKHRadioResolved(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnKHRadioResolveDone(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnKHRadioPrepared(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnKHRadioDone(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnKHRadioCover(WPARAM wParam, LPARAM lParam);

private:
	struct LayoutItem { HWND window; CRect rect; };
	std::vector<LayoutItem> m_layout;
	CSize m_templateSize = CSize(0, 0);
	LOGFONT m_templateFont = {};
	CFont m_scaledFont;
	UINT m_templateDpi = 96;
	UINT m_layoutDpi = 96;

	struct FetchParams;
	struct ResolveParams;
	struct CoverParams;
	struct PrepareParams;
	static void FetchThread(std::unique_ptr<FetchParams> p);
	static void ResolveThread(std::unique_ptr<ResolveParams> p);
	static void PrepareThread(std::unique_ptr<PrepareParams> p);
	static void CoverThread(std::unique_ptr<CoverParams> p);

	// Worker threads are joined at exit (with a time limit), and finished ones
	// are reaped whenever a new one starts.
	struct Worker {
		std::thread thread;
		std::shared_ptr<std::atomic<bool>> done;
	};
	template <class Fn> bool StartWorker(LPCWSTR what, Fn&& fn);
	void ReapWorkers();
	void JoinWorkers(DWORD timeoutMs);
	std::vector<Worker> m_workers;

	void Relayout(int cx, int cy);
	// shared: the request came from a pasted link; startAt: its track (see KHShare::Link::trackKey)
	void StartFetch(bool bDirect, const CStringW& albumUrl, bool bAppend = false, bool shared = false, const CStringW& startAt = CStringW());
	void StartCoverFetch(const CStringW& albumUrl);  // fetch just the cover for a resumed/restored album
	void MaybePrefetchNext(); // keep KHRADIO_QUEUE_DEPTH albums queued ahead
	void KickResolver();      // look up audio for the next few unresolved tracks
	// Prepare (download, decode) the first Ogg / fallback-m4a track among the
	// playing one and the next few; the one the player waits for goes first.
	void KickPreparer();
	void PruneCache();        // drop cache files of tracks no longer current or upcoming
	void ShowPreparing(const QueueItem& item);
	unsigned ScheduleFetchRetry(KHInsider::FetchStatus why); // seconds until it runs (0 = when signed in)
	void Watchdog();
	int CountQueuedAlbums() const; // albums from the playing one to the end of the queue (playable rows)
	// Applies the album context to a track's audio decision, logs it, and marks
	// the track skipped when the filter drops it. True if it was skipped.
	bool ApplyAudioCheck(int index, const KHAudio::Decision& d, LPCWSTR route);
	void PopulateFilterLists();
	void RestoreSelections();
	void SaveSelections();
	void RefreshHistoryList();
	void HistoryRecorded(int previousIndex);
	CStringW SessionStatusText(int status, LPARAM lParam) const;
	static CStringW HistoryText(const CKHRadioHistory::AlbumEntry& album);
	static CStringW JoinSelection(CListBox& list);
	static void ApplySelection(CListBox& list, const CStringW& csv);
	static void GetSelectedValues(CListBox& list, std::vector<int>& values);

	IKHRadioHost* m_host = nullptr;
	std::shared_ptr<std::atomic<UINT>> m_pGen = std::make_shared<std::atomic<UINT>>(0);        // album fetches
	std::shared_ptr<std::atomic<UINT>> m_resolveGen = std::make_shared<std::atomic<UINT>>(0);  // audio lookups
	bool m_bFetching = false;
	ULONGLONG m_fetchStarted = 0;
	bool m_bResolving = false;
	ULONGLONG m_resolveStarted = 0;
	ULONGLONG m_fetchRetryAt = 0;    // a failed radio fetch is retried at this tick (0 = none)
	unsigned m_fetchRetryDelay = 0;  // seconds; grows on repeated failures
	ULONGLONG m_resolveRetryAt = 0;
	std::shared_ptr<std::atomic<UINT>> m_prepareGen = std::make_shared<std::atomic<UINT>>(0); // audio cache work
	bool m_bPreparing = false;
	UINT64 m_prepareId = 0;          // queue item being prepared
	ULONGLONG m_prepareStarted = 0;
	ULONGLONG m_prepareRetryAt = 0;

	// A fetch to run later: at m_fetchRetryAt, or (when that is 0) once the
	// session is Ready again.
	bool m_pendingStart = false;
	bool m_pendingDirect = false;
	bool m_pendingAppend = false;
	CStringW m_pendingUrl;
	bool m_pendingShared = false;
	CStringW m_pendingStartAt;
	// the fetch in flight, so a failure can be retried as it was asked
	bool m_fetchDirect = false;
	bool m_fetchAppend = false;
	CStringW m_fetchUrl;
	bool m_fetchShared = false;
	CStringW m_fetchStartAt;
	CStringW m_shareNotice;          // shown with the first track of a shared album (track not found)
	bool m_sessionNotice = false;    // the status line shows a session problem
	void RunPendingFetch();

	// light/dark palette (follows the Windows app mode)
	bool m_bDarkTheme = false;
	CBrush m_brushWindow;
	CBrush m_brushList;

	CStringW m_lastAlbumTitle;

	// continuous radio: keep a couple of albums queued ahead so playback
	// never runs dry while the next album is being fetched.
	bool m_bRadioActive = false;        // a radio session is running
	CStringW m_currentPlayingAlbum;     // album of the track currently playing
	std::vector<CStringW> m_coverFetches; // albums with a cover fetch in flight
	std::map<CStringW, KHAudio::AlbumTally> m_albumTally; // talking-track filter: what each album sounded like so far

	CKHRadioHistory m_history;
};
