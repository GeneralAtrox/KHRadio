// KH Radio - main window: radio controls (CKHRadioDlg) on the left, now playing
// (cover, track, seek, transport, volume) and the upcoming queue on the right.
// GPL-3.0-or-later (see COPYING.txt).
#pragma once

#include <vector>
#include "KHRadioDlg.h"
#include "MediaSession.h"
#include "Player.h"
#include "UiKit.h"

class CMainWnd : public CWnd, public IPlayerListener, public IKHRadioHost
{
public:
	CMainWnd() = default;
	BOOL CreateMain();
	// Ctrl+V (play a shared link) and Tab across the whole window; called by the
	// app before the message is dispatched. True if the message was used.
	bool TranslateShortcut(MSG* msg);

	// IKHRadioHost
	CRadioPlayer& Player() override { return m_player; }
	void ShowAlbumCover(const CStringW& albumUrl, const CStringW& coverPath) override;

	// IPlayerListener
	void OnTrackStarted(int index, bool resumed) override;
	void OnQueueEnded() override;
	void OnPlayerError(int index, const CStringW& message) override;
	void OnQueueChanged() override;
	void OnStateChanged() override;
	void OnNeedResolve(int index) override;

protected:
	void PostNcDestroy() override { delete this; }
	DECLARE_MESSAGE_MAP()

	afx_msg int OnCreate(LPCREATESTRUCT cs);
	afx_msg void OnClose();
	afx_msg void OnDestroy();
	afx_msg void OnSize(UINT type, int cx, int cy);
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC*) { return TRUE; }
	afx_msg void OnGetMinMaxInfo(MINMAXINFO* mmi);
	afx_msg void OnTimer(UINT_PTR id);
	afx_msg HBRUSH OnCtlColor(CDC* dc, CWnd* wnd, UINT ctlColor);
	afx_msg void OnMeasureItem(int id, LPMEASUREITEMSTRUCT mis);
	afx_msg void OnDrawItem(int id, LPDRAWITEMSTRUCT dis);
	afx_msg void OnSettingChange(UINT flags, LPCTSTR section);
	afx_msg void OnSysColorChange();
	afx_msg void OnPrev();
	afx_msg void OnPlayPause();
	afx_msg void OnNext();
	afx_msg void OnQueueDblClk();
	afx_msg void OnShare();
	afx_msg void OnPlayShared();
	afx_msg void OnContextMenu(CWnd* wnd, CPoint pt);
	afx_msg LRESULT OnPlayerEvent(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnSlider(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnDpiChanged(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnAppCommand(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnMediaButton(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnIconicThumbnail(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnIconicLivePreview(WPARAM wParam, LPARAM lParam);

private:
	enum class MediaCmd { PlayPause, Play, Pause, Next, Prev };
	int S(int px) const { return MulDiv(px, m_dpi, 96); }
	void CreateFonts();
	void Layout();
	void ApplyTheme();
	void RefreshQueueList();
	void UpdateTitle();
	void UpdateTimes();
	void UpdateIconic();
	void UpdateMediaTrack();
	void PruneCovers(bool startup);
	void MediaCommand(MediaCmd cmd, bool fromMediaControls);
	void SaveState();
	void UpdateShareButton();
	void ShareText(const CStringW& text, LPCWSTR what);

	CRadioPlayer m_player;
	CKHRadioDlg m_dlg;
	CSliderBar m_seek;
	CSliderBar m_volume;
	CGlyphButton m_prev, m_play, m_next;
	CGlyphButton m_share, m_playShared;
	CToolTipCtrl m_tips;
	CListBox m_queue;
	CMediaSession m_media;

	CFont m_fontUI, m_fontUIBold, m_fontTitle, m_fontGlyph, m_fontGlyphBig, m_fontGlyphHuge;
	CBrush m_brushList;

	CCoverImage m_cover;
	HBITMAP m_coverScaled = nullptr;
	CSize m_coverScaledSize;
	CSize m_coverScaledFor;

	CRect m_rcCover, m_rcTitle, m_rcAlbum, m_rcPlatforms, m_rcElapsed, m_rcTotal, m_rcVolumeIcon, m_rcQueueLabel;
	UINT m_dpi = 96;
	std::vector<UINT64> m_listedIds; // queue items shown in the list, in order
	int m_listedCurrent = -2;
	int m_tick = 0;
	ULONGLONG m_lastMediaTick = 0;
	bool m_lastMediaFromControls = false;
	bool m_stateSaved = false;
};
