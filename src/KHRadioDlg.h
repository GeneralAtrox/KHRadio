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

#pragma once

#include <afxwin.h>
#include <atomic>
#include <memory>
#include <vector>
#include "KHInsider.h"
#include "RadioTypes.h"

class CRadioPlayer;

#define WM_KHRADIO_STATUS (WM_APP + 850)
#define WM_KHRADIO_ALBUM  (WM_APP + 851)
#define WM_KHRADIO_TRACK  (WM_APP + 852)
#define WM_KHRADIO_DONE   (WM_APP + 853)
#define WM_KHRADIO_COVER  (WM_APP + 854)

// What the dialog needs from the main window (replaces CMainFrame).
struct IKHRadioHost {
	virtual CRadioPlayer& Player() = 0;
	// Show this album's cover if the album is the one playing (empty path = placeholder).
	virtual void ShowAlbumCover(const CStringW& albumUrl, const CStringW& coverPath) = 0;
};

// CKHRadioDlg dialog — replica of the site's "Random Album Advanced" form
// plus listening history.

class CKHRadioDlg : public CDialog
{
public:
	CKHRadioDlg();
	virtual ~CKHRadioDlg();

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

	// called when the queue runs out; returns true if the radio is taking over
	// (fetching the next album) instead of stopping.
	bool ContinueRadioAtEnd();
	void ScaleForDPI(UINT dpi);
	CSize GetNaturalSize() const;
	int GetMinimumHeight() const;
	void ApplyTheme();
	void SetStatus(const CStringW& text);

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	virtual BOOL OnInitDialog();
	virtual void OnOK() {}      // child dialog: Enter/Esc must not destroy it
	virtual void OnCancel() {}
	DECLARE_MESSAGE_MAP()

	afx_msg void OnDestroy();
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);
	afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct);
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
	afx_msg LRESULT OnKHRadioStatus(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnKHRadioAlbum(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnKHRadioTrack(WPARAM wParam, LPARAM lParam);
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
	struct FetchParams {
		HWND hWnd = nullptr;
		UINT gen = 0;
		std::shared_ptr<std::atomic<UINT>> pGen; // shared with the dialog; mismatch = cancelled
		bool bDirect = false;              // true: fetch m_albumUrl, false: POST random form
		bool bAppend = false;              // continuous radio: append to the queue instead of replacing it
		CStringA formBody;
		CStringW albumUrl;
		bool bAvoidPlayed = false;
		bool bFilterSpoken = false;
		bool bExclusive = false;            // only keep albums exclusive to the selected console(s)
		std::vector<CStringW> selectedPlatformNames; // display names of the chosen consoles
		std::vector<CStringW> playedAlbums; // snapshot of history URLs
	};

	static UINT FetchThreadProc(LPVOID pParam);
	static UINT CoverFetchThreadProc(LPVOID pParam); // background cover-only fetch for an album without one

	void Relayout(int cx, int cy);
	void StartFetch(bool bDirect, const CStringW& albumUrl, bool bAppend = false);
	void StartCoverFetch(const CStringW& albumUrl);  // fetch just the cover for a resumed/restored album
	void MaybePrefetchNext(); // keep KHRADIO_QUEUE_DEPTH albums queued ahead
	int CountQueuedAlbums() const; // albums from the playing one to the end of the queue
	void PopulateFilterLists();
	void RestoreSelections();
	void SaveSelections();
	void RefreshHistoryList();
	static CStringW JoinSelection(CListBox& list);
	static void ApplySelection(CListBox& list, const CStringW& csv);
	static void GetSelectedValues(CListBox& list, std::vector<int>& values);

	IKHRadioHost* m_host = nullptr;
	std::shared_ptr<std::atomic<UINT>> m_pGen = std::make_shared<std::atomic<UINT>>(0);
	bool m_bFetching = false;

	// light/dark palette (follows the Windows app mode)
	bool m_bDarkTheme = false;
	CBrush m_brushWindow;
	CBrush m_brushList;

	KHInsider::Album m_currentAlbum;
	bool m_bPlaylistStarted = false;

	// continuous radio: keep a couple of albums queued ahead so playback
	// never runs dry while the next album is being fetched/analyzed.
	bool m_bRadioActive = false;        // a radio session is running
	CStringW m_currentPlayingAlbum;     // album of the track currently playing
	std::vector<CStringW> m_coverFetches; // albums with a cover fetch in flight

	CKHRadioHistory m_history;
};
