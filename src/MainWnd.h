// KH Radio - main window: radio controls (CKHRadioDlg) on the left, now playing
// (cover, track, seek, transport, volume) and the upcoming queue on the right.
// GPL-3.0-or-later (see COPYING.txt).
#pragma once

#include "KHRadioDlg.h"
#include "Player.h"
#include "UiKit.h"

class CMainWnd : public CWnd, public IPlayerListener, public IKHRadioHost
{
public:
	CMainWnd() = default;
	BOOL CreateMain();

	// IKHRadioHost
	CRadioPlayer& Player() override { return m_player; }
	void ShowAlbumCover(const CStringW& albumUrl, const CStringW& coverPath) override;

	// IPlayerListener
	void OnTrackStarted(int index, bool resumed) override;
	void OnTrackEnded(int index) override;
	void OnQueueEnded() override;
	void OnPlayerError(int index, const CStringW& message) override;
	void OnQueueChanged() override;
	void OnStateChanged() override;

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
	afx_msg LRESULT OnPlayerEvent(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnSlider(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnDpiChanged(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnAppCommand(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnHotKey(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnIconicThumbnail(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnIconicLivePreview(WPARAM wParam, LPARAM lParam);

private:
	int S(int px) const { return MulDiv(px, m_dpi, 96); }
	void CreateFonts();
	void Layout();
	void ApplyTheme();
	void RefreshQueueList();
	void UpdateTitle();
	void UpdateTimes();
	void UpdateIconic();
	void PruneCovers(bool startup);
	void RegisterMediaKeys();
	void SaveState();

	CRadioPlayer m_player;
	CKHRadioDlg m_dlg;
	CSliderBar m_seek;
	CSliderBar m_volume;
	CGlyphButton m_prev, m_play, m_next;
	CListBox m_queue;

	CFont m_fontUI, m_fontUIBold, m_fontTitle, m_fontGlyph, m_fontGlyphBig, m_fontGlyphHuge;
	CBrush m_brushList;

	CCoverImage m_cover;
	CStringW m_coverAlbum;
	HBITMAP m_coverScaled = nullptr;
	CSize m_coverScaledSize;
	CSize m_coverScaledFor;

	CRect m_rcCover, m_rcTitle, m_rcAlbum, m_rcPlatforms, m_rcElapsed, m_rcTotal, m_rcVolumeIcon, m_rcQueueLabel;
	UINT m_dpi = 96;
	int m_listedCurrent = -2;
	int m_tick = 0;
	bool m_hotkeys = false;
	bool m_stateSaved = false;
};
