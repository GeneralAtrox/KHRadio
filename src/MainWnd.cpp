// KH Radio - main window: radio controls (CKHRadioDlg) on the left, now playing
// (cover, track, seek, transport, volume) and the upcoming queue on the right.
// GPL-3.0-or-later (see COPYING.txt).
#include "stdafx.h"
#include "MainWnd.h"
#include "AppData.h"
#include "Log.h"

#include <dwmapi.h>
#include <set>

#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace
{
	constexpr UINT_PTR kUiTimer = 1;
	constexpr UINT kUiTimerMs = 250;

	// Segoe MDL2 Assets glyphs
	constexpr wchar_t kGlyphPrev = 0xE892;
	constexpr wchar_t kGlyphNext = 0xE893;
	constexpr wchar_t kGlyphPlay = 0xE768;
	constexpr wchar_t kGlyphPause = 0xE769;
	constexpr wchar_t kGlyphVolume = 0xE767;
	constexpr wchar_t kGlyphMute = 0xE74F;
	constexpr wchar_t kGlyphMusic = 0xE8D6;

	// One key press can arrive both through the media controls and as
	// WM_APPCOMMAND; the second copy within this time is ignored.
	constexpr ULONGLONG kMediaDebounceMs = 300;

	CStringW QueueText(const QueueItem& item)
	{
		CStringW s = item.label;
		if (!item.ref.albumTitle.IsEmpty()) {
			s += L"   \x2014   " + item.ref.albumTitle;
		}
		return s;
	}

	bool FileHasData(const CStringW& path)
	{
		WIN32_FILE_ATTRIBUTE_DATA fa = {};
		return !path.IsEmpty() && GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && (fa.nFileSizeLow || fa.nFileSizeHigh);
	}

	CStringW FormatTime(double seconds)
	{
		if (!(seconds >= 0.0)) {
			seconds = 0.0;
		}
		const int total = static_cast<int>(seconds);
		CStringW s;
		if (total >= 3600) {
			s.Format(L"%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
		} else {
			s.Format(L"%d:%02d", total / 60, total % 60);
		}
		return s;
	}

	void ForceOpaque(void* bits, int width, int height)
	{
		auto* px = static_cast<BYTE*>(bits);
		for (int i = 0; i < width * height; i++) {
			px[i * 4 + 3] = 255;
		}
	}

	HBITMAP CreateDib32(int width, int height, void** bits)
	{
		BITMAPINFO bi = {};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = width;
		bi.bmiHeader.biHeight = -height;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		return CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
	}
}

BEGIN_MESSAGE_MAP(CMainWnd, CWnd)
	ON_WM_CREATE()
	ON_WM_CLOSE()
	ON_WM_DESTROY()
	ON_WM_SIZE()
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_GETMINMAXINFO()
	ON_WM_TIMER()
	ON_WM_CTLCOLOR()
	ON_WM_MEASUREITEM()
	ON_WM_DRAWITEM()
	ON_WM_SETTINGCHANGE()
	ON_WM_SYSCOLORCHANGE()
	ON_BN_CLICKED(IDC_KHRADIO_PREV, OnPrev)
	ON_BN_CLICKED(IDC_KHRADIO_PLAYPAUSE, OnPlayPause)
	ON_BN_CLICKED(IDC_KHRADIO_NEXT, OnNext)
	ON_LBN_DBLCLK(IDC_KHRADIO_QUEUE, OnQueueDblClk)
	ON_MESSAGE(WM_KHRADIO_PLAYER_EVENT, OnPlayerEvent)
	ON_MESSAGE(WM_KHRADIO_SLIDER, OnSlider)
	ON_MESSAGE(WM_DPICHANGED, OnDpiChanged)
	ON_MESSAGE(WM_APPCOMMAND, OnAppCommand)
	ON_MESSAGE(WM_KHRADIO_SMTC, OnMediaButton)
	ON_MESSAGE(WM_DWMSENDICONICTHUMBNAIL, OnIconicThumbnail)
	ON_MESSAGE(WM_DWMSENDICONICLIVEPREVIEWBITMAP, OnIconicLivePreview)
END_MESSAGE_MAP()

BOOL CMainWnd::CreateMain()
{
	WNDCLASSW wc = {};
	wc.style = CS_DBLCLKS;
	wc.lpfnWndProc = ::DefWindowProcW;
	wc.hInstance = AfxGetInstanceHandle();
	wc.hIcon = ::LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDR_MAINFRAME));
	wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = L"KHRadio";
	if (!AfxRegisterClass(&wc)) {
		return FALSE;
	}

	const UINT sysDpi = GetDpiForSystem();
	if (!CreateEx(0, L"KHRadio", L"KH Radio", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
			CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1040, sysDpi, 96), MulDiv(880, sysDpi, 96), nullptr, nullptr)) {
		return FALSE;
	}

	const auto& s = KHRadioData::Get();
	bool maximized = false;
	if (s.hasPlacement && MonitorFromRect(&s.normalRect, MONITOR_DEFAULTTONULL)) {
		WINDOWPLACEMENT wp = { sizeof(wp) };
		GetWindowPlacement(&wp);
		wp.rcNormalPosition = s.normalRect;
		wp.showCmd = SW_HIDE;
		SetWindowPlacement(&wp);
		maximized = s.maximized;
	}
	ShowWindow(maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
	UpdateWindow();
	return TRUE;
}

int CMainWnd::OnCreate(LPCREATESTRUCT cs)
{
	if (CWnd::OnCreate(cs) == -1) {
		return -1;
	}
	m_dpi = GetDpiForWindow(m_hWnd);
	const HINSTANCE inst = AfxGetInstanceHandle();
	SetIcon(static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDR_MAINFRAME), IMAGE_ICON,
		GetSystemMetricsForDpi(SM_CXICON, m_dpi), GetSystemMetricsForDpi(SM_CYICON, m_dpi), 0)), TRUE);
	SetIcon(static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDR_MAINFRAME), IMAGE_ICON,
		GetSystemMetricsForDpi(SM_CXSMICON, m_dpi), GetSystemMetricsForDpi(SM_CYSMICON, m_dpi), 0)), FALSE);
	CreateFonts();

	const bool audioOk = m_player.Initialize(m_hWnd, this);
	m_player.SetVolume(KHRadioData::Get().volume);

	m_seek.Create(this, IDC_KHRADIO_SEEK);
	m_volume.Create(this, IDC_KHRADIO_VOLUME);
	m_volume.SetValue(m_player.Volume());
	m_prev.Create(this, IDC_KHRADIO_PREV, kGlyphPrev, false);
	m_play.Create(this, IDC_KHRADIO_PLAYPAUSE, kGlyphPlay, true);
	m_next.Create(this, IDC_KHRADIO_NEXT, kGlyphNext, false);
	m_prev.SetGlyphFont(&m_fontGlyph);
	m_play.SetGlyphFont(&m_fontGlyphBig);
	m_next.SetGlyphFont(&m_fontGlyph);
	m_queue.CreateEx(0, L"LISTBOX", nullptr,
		WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
		CRect(0, 0, 10, 10), this, IDC_KHRADIO_QUEUE);
	m_queue.SetFont(&m_fontUI);

	if (!m_dlg.Create(this, this)) {
		return -1;
	}
	m_dlg.ShowWindow(SW_SHOW);
	if (!audioOk) {
		m_dlg.SetStatus(L"Audio playback is unavailable (Media Foundation could not start).");
	}

	ApplyTheme();
	Layout();

	// resume the queue saved at the last exit, where it left off
	const auto saved = KHRadioData::LoadQueue();
	for (const auto& item : saved.items) {
		m_player.Append(item);
	}
	PruneCovers(true);
	// Media keys, the volume flyout and the lock screen go through Windows'
	// media controls; WM_APPCOMMAND still covers the focused window.
	m_media.Initialize(m_hWnd);

	if (saved.index >= 0 && audioOk) {
		KHLog::Format(L"resume: %u queued item(s), playing #%d from %.0f s", static_cast<unsigned>(saved.items.size()), saved.index, saved.position);
		m_player.PlayIndex(saved.index, saved.position, true);
	}

	SetTimer(kUiTimer, kUiTimerMs, nullptr);
	return 0;
}

void CMainWnd::CreateFonts()
{
	NONCLIENTMETRICSW ncm = { sizeof(ncm) };
	SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, m_dpi);
	LOGFONTW lf = ncm.lfMessageFont;
	for (CFont* f : { &m_fontUI, &m_fontUIBold, &m_fontTitle, &m_fontGlyph, &m_fontGlyphBig, &m_fontGlyphHuge }) {
		f->DeleteObject();
	}
	m_fontUI.CreateFontIndirectW(&lf);
	lf.lfWeight = FW_SEMIBOLD;
	m_fontUIBold.CreateFontIndirectW(&lf);
	lf.lfHeight = -S(22);
	m_fontTitle.CreateFontIndirectW(&lf);

	LOGFONTW g = {};
	g.lfCharSet = DEFAULT_CHARSET;
	g.lfQuality = CLEARTYPE_QUALITY;
	wcscpy_s(g.lfFaceName, L"Segoe MDL2 Assets");
	g.lfHeight = -S(15);
	m_fontGlyph.CreateFontIndirectW(&g);
	g.lfHeight = -S(20);
	m_fontGlyphBig.CreateFontIndirectW(&g);
	g.lfHeight = -S(72);
	m_fontGlyphHuge.CreateFontIndirectW(&g);
}

void CMainWnd::ApplyTheme()
{
	const auto& p = KHTheme::Current();
	KHTheme::ApplyTitleBar(m_hWnd);
	m_brushList.DeleteObject();
	m_brushList.CreateSolidBrush(p.listBk);
	if (m_queue.m_hWnd) {
		m_queue.ModifyStyleEx(p.dark ? WS_EX_CLIENTEDGE : 0, p.dark ? 0 : WS_EX_CLIENTEDGE);
		m_queue.ModifyStyle(p.dark ? 0 : WS_BORDER, p.dark ? WS_BORDER : 0);
		m_queue.SetWindowPos(nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
		KHTheme::ApplyToScrollable(m_queue.m_hWnd);
	}
	if (m_dlg.m_hWnd) {
		m_dlg.ApplyTheme();
	}
	RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void CMainWnd::OnSettingChange(UINT flags, LPCTSTR section)
{
	CWnd::OnSettingChange(flags, section);
	if (section && wcscmp(section, L"ImmersiveColorSet") == 0) {
		const bool wasDark = KHTheme::Current().dark;
		KHTheme::Refresh();
		if (KHTheme::Current().dark != wasDark) {
			ApplyTheme();
		}
	}
}

void CMainWnd::OnSysColorChange()
{
	CWnd::OnSysColorChange();
	KHTheme::Refresh();
	ApplyTheme();
}

void CMainWnd::Layout()
{
	if (!m_dlg.m_hWnd) {
		return;
	}
	CRect rc;
	GetClientRect(rc);
	const CSize dlg = m_dlg.GetNaturalSize();
	m_dlg.MoveWindow(0, 0, dlg.cx, rc.Height());

	const int m = S(14);
	CRect panel(dlg.cx + m, m, rc.right - m, rc.bottom - m);
	if (panel.Width() < S(100)) {
		panel.right = panel.left + S(100);
	}
	const int pw = panel.Width();

	// cover: a square up to ~45% of the height
	const int cs = std::max(S(96), std::min(pw, panel.Height() * 45 / 100));
	m_rcCover = CRect(CPoint(panel.left + (pw - cs) / 2, panel.top), CSize(cs, cs));
	int y = m_rcCover.bottom + S(10);
	m_rcTitle = CRect(panel.left, y, panel.right, y + S(30)); y += S(30);
	m_rcAlbum = CRect(panel.left, y, panel.right, y + S(20)); y += S(20);
	m_rcPlatforms = CRect(panel.left, y, panel.right, y + S(20)); y += S(26);

	const int timeW = S(56);
	m_rcElapsed = CRect(panel.left, y, panel.left + timeW, y + S(20));
	m_rcTotal = CRect(panel.right - timeW, y, panel.right, y + S(20));
	m_seek.MoveWindow(panel.left + timeW + S(4), y, pw - 2 * timeW - S(8), S(20));
	y += S(28);

	const int b = S(40), bb = S(48);
	const int cx = panel.left + pw / 2;
	m_play.MoveWindow(cx - bb / 2, y, bb, bb);
	m_prev.MoveWindow(cx - bb / 2 - S(12) - b, y + (bb - b) / 2, b, b);
	m_next.MoveWindow(cx + bb / 2 + S(12), y + (bb - b) / 2, b, b);
	const int volW = std::min(S(110), std::max(S(40), pw / 2 - bb / 2 - b - S(60)));
	m_volume.MoveWindow(panel.right - volW, y + (bb - S(20)) / 2, volW, S(20), TRUE);
	m_rcVolumeIcon = CRect(panel.right - volW - S(26), y + (bb - S(20)) / 2, panel.right - volW - S(4), y + (bb + S(20)) / 2);
	y += bb + S(12);

	m_rcQueueLabel = CRect(panel.left, y, panel.right, y + S(20));
	y += S(22);
	m_queue.MoveWindow(panel.left, y, pw, std::max<int>(S(40), panel.bottom - y));
	Invalidate(FALSE);
}

void CMainWnd::OnSize(UINT type, int cx, int cy)
{
	CWnd::OnSize(type, cx, cy);
	if (type != SIZE_MINIMIZED) {
		Layout();
	}
}

void CMainWnd::OnGetMinMaxInfo(MINMAXINFO* mmi)
{
	CWnd::OnGetMinMaxInfo(mmi);
	if (!m_dlg.m_hWnd) {
		return;
	}
	CRect r(0, 0, m_dlg.GetNaturalSize().cx + S(360), std::max(m_dlg.GetMinimumHeight(), S(600)));
	AdjustWindowRectExForDpi(&r, GetStyle(), FALSE, GetExStyle(), m_dpi);
	mmi->ptMinTrackSize.x = r.Width();
	mmi->ptMinTrackSize.y = r.Height();
}

LRESULT CMainWnd::OnDpiChanged(WPARAM wParam, LPARAM lParam)
{
	m_dpi = HIWORD(wParam);
	CreateFonts();
	m_queue.SetFont(&m_fontUI);
	m_queue.SetItemHeight(0, S(24));
	m_dlg.ScaleForDPI(m_dpi);
	if (m_coverScaled) {
		DeleteObject(m_coverScaled);
		m_coverScaled = nullptr;
	}
	const auto* r = reinterpret_cast<const RECT*>(lParam);
	SetWindowPos(nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
	Layout();
	RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
	return 0;
}

HBRUSH CMainWnd::OnCtlColor(CDC* dc, CWnd* wnd, UINT ctlColor)
{
	if (ctlColor == CTLCOLOR_LISTBOX) {
		const auto& p = KHTheme::Current();
		dc->SetTextColor(p.listText);
		dc->SetBkColor(p.listBk);
		return m_brushList;
	}
	return CWnd::OnCtlColor(dc, wnd, ctlColor);
}

void CMainWnd::OnMeasureItem(int id, LPMEASUREITEMSTRUCT mis)
{
	if (id == IDC_KHRADIO_QUEUE) {
		mis->itemHeight = S(24);
		return;
	}
	CWnd::OnMeasureItem(id, mis);
}

void CMainWnd::OnDrawItem(int id, LPDRAWITEMSTRUCT dis)
{
	if (id != IDC_KHRADIO_QUEUE) {
		CWnd::OnDrawItem(id, dis); // reflected to the glyph buttons
		return;
	}
	if (dis->itemID == static_cast<UINT>(-1)) {
		return;
	}
	const auto& p = KHTheme::Current();
	CDC* dc = CDC::FromHandle(dis->hDC);
	CRect r(dis->rcItem);
	const int item = static_cast<int>(dis->itemID);
	const int current = m_player.CurrentIndex();
	const bool selected = !!(dis->itemState & ODS_SELECTED);

	dc->FillSolidRect(r, selected ? p.selBk : p.listBk);
	COLORREF color = p.listText;
	if (selected) {
		color = p.selText;
	} else if (item == current) {
		color = p.accent;
	} else if (item < current) {
		color = p.subtext;
	}
	CStringW text;
	m_queue.GetText(item, text);
	if (item == current) {
		text = L"\x25B6  " + text;
	}
	CFont* old = dc->SelectObject(item == current ? &m_fontUIBold : &m_fontUI);
	dc->SetBkMode(TRANSPARENT);
	dc->SetTextColor(color);
	CRect tr(r);
	tr.DeflateRect(S(8), 0);
	dc->DrawTextW(text, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
	dc->SelectObject(old);
	if (dis->itemState & ODS_FOCUS) {
		dc->DrawFocusRect(r);
	}
}

void CMainWnd::OnPaint()
{
	CPaintDC paint(this);
	CRect rc;
	GetClientRect(rc);
	if (rc.IsRectEmpty()) {
		return;
	}
	CDC dc;
	dc.CreateCompatibleDC(&paint);
	CBitmap bmp;
	bmp.CreateCompatibleBitmap(&paint, rc.Width(), rc.Height());
	CBitmap* oldBmp = dc.SelectObject(&bmp);
	const auto& p = KHTheme::Current();
	dc.FillSolidRect(rc, p.window);
	dc.SetBkMode(TRANSPARENT);

	// cover, aspect preserved, centred in its square
	if (m_cover.IsLoaded()) {
		if (!m_coverScaled || m_coverScaledFor != m_rcCover.Size()) {
			if (m_coverScaled) {
				DeleteObject(m_coverScaled);
			}
			SIZE actual = {};
			m_coverScaled = m_cover.CreateScaled(m_rcCover.Size(), &actual);
			m_coverScaledSize = actual;
			m_coverScaledFor = m_rcCover.Size();
		}
	}
	if (m_cover.IsLoaded() && m_coverScaled) {
		CDC src;
		src.CreateCompatibleDC(&dc);
		HGDIOBJ old = SelectObject(src.m_hDC, m_coverScaled);
		const int x = m_rcCover.left + (m_rcCover.Width() - m_coverScaledSize.cx) / 2;
		const int y = m_rcCover.top + (m_rcCover.Height() - m_coverScaledSize.cy) / 2;
		BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
		AlphaBlend(dc.m_hDC, x, y, m_coverScaledSize.cx, m_coverScaledSize.cy, src.m_hDC, 0, 0, m_coverScaledSize.cx, m_coverScaledSize.cy, bf);
		SelectObject(src.m_hDC, old);
	} else {
		dc.FillSolidRect(m_rcCover, p.placeholder);
		CFont* old = dc.SelectObject(&m_fontGlyphHuge);
		dc.SetTextColor(p.subtext);
		const wchar_t glyph[2] = { kGlyphMusic, 0 };
		dc.DrawTextW(glyph, 1, m_rcCover, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		dc.SelectObject(old);
	}

	const QueueItem* cur = m_player.Current();
	const CStringW title = cur ? cur->ref.name : CStringW(L"Nothing playing");
	const CStringW album = cur ? cur->ref.albumTitle : CStringW(L"Roll a random album, or double-click one in the history.");
	const CStringW platforms = (cur && !cur->ref.platforms.IsEmpty()) ? L"Platforms: " + cur->ref.platforms : CStringW();
	const UINT fmt = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;

	CFont* oldFont = dc.SelectObject(&m_fontTitle);
	dc.SetTextColor(p.text);
	dc.DrawTextW(title, CRect(m_rcTitle), fmt);
	dc.SelectObject(&m_fontUI);
	dc.DrawTextW(album, CRect(m_rcAlbum), fmt);
	dc.SetTextColor(p.subtext);
	dc.DrawTextW(platforms, CRect(m_rcPlatforms), fmt);

	// times
	const double duration = m_player.Duration();
	double position = m_player.Position();
	if (m_seek.IsDragging() && duration > 0.0) {
		position = m_seek.GetValue() * duration;
	}
	dc.SetTextColor(p.text);
	dc.DrawTextW(cur ? FormatTime(position) : CStringW(L"0:00"), CRect(m_rcElapsed), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	dc.DrawTextW(duration > 0.0 ? FormatTime(duration) : CStringW(L"--:--"), CRect(m_rcTotal), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

	// volume icon
	dc.SelectObject(&m_fontGlyph);
	const wchar_t vol[2] = { m_player.Volume() <= 0.0 ? kGlyphMute : kGlyphVolume, 0 };
	dc.DrawTextW(vol, 1, CRect(m_rcVolumeIcon), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	// queue heading
	dc.SelectObject(&m_fontUIBold);
	CStringW heading;
	const int total = static_cast<int>(m_player.Queue().size());
	const int ahead = cur ? total - m_player.CurrentIndex() - 1 : total;
	heading.Format(L"Queue - %d up next (double-click to jump)", std::max(0, ahead));
	dc.DrawTextW(heading, CRect(m_rcQueueLabel), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	dc.SelectObject(oldFont);

	paint.BitBlt(0, 0, rc.Width(), rc.Height(), &dc, 0, 0, SRCCOPY);
	dc.SelectObject(oldBmp);
}

void CMainWnd::UpdateTimes()
{
	InvalidateRect(m_rcElapsed, FALSE);
	InvalidateRect(m_rcTotal, FALSE);
}

void CMainWnd::OnTimer(UINT_PTR id)
{
	if (id != kUiTimer) {
		CWnd::OnTimer(id);
		return;
	}
	m_player.Tick();
	const double duration = m_player.Duration();
	m_seek.SetValue(duration > 0.0 ? m_player.Position() / duration : 0.0);
	UpdateTimes();
	// keep the resume point fresh in case the app is killed
	if (++m_tick % 60 == 0 && m_player.GetState() == CRadioPlayer::State::Playing) {
		KHRadioData::SaveQueue(m_player.Queue(), m_player.CurrentIndex(), m_player.Position());
	}
}

LRESULT CMainWnd::OnSlider(WPARAM wParam, LPARAM lParam)
{
	if (wParam == IDC_KHRADIO_SEEK) {
		if (lParam) {
			m_player.Seek(m_seek.GetValue() * m_player.Duration());
		}
		UpdateTimes();
	} else if (wParam == IDC_KHRADIO_VOLUME) {
		m_player.SetVolume(m_volume.GetValue());
		KHRadioData::Get().volume = m_player.Volume();
		if (lParam) {
			KHRadioData::Save();
		}
		InvalidateRect(m_rcVolumeIcon, FALSE);
	}
	return 0;
}

void CMainWnd::OnPrev()
{
	m_player.Prev();
}

void CMainWnd::OnPlayPause()
{
	if (m_player.Queue().empty()) {
		m_dlg.SendMessageW(WM_COMMAND, MAKEWPARAM(IDC_KHRADIO_RANDOM_BUTTON, BN_CLICKED), 0);
		return;
	}
	m_player.TogglePause();
}

void CMainWnd::OnNext()
{
	m_player.Next();
}

void CMainWnd::OnQueueDblClk()
{
	const int sel = m_queue.GetCurSel();
	if (sel != LB_ERR) {
		m_player.PlayIndex(sel);
	}
}

void CMainWnd::MediaCommand(MediaCmd cmd, bool fromMediaControls)
{
	const ULONGLONG now = GetTickCount64();
	if (fromMediaControls != m_lastMediaFromControls && now - m_lastMediaTick < kMediaDebounceMs) {
		return; // the same key press, seen through the other path
	}
	m_lastMediaTick = now;
	m_lastMediaFromControls = fromMediaControls;
	switch (cmd) {
		case MediaCmd::PlayPause: OnPlayPause(); break;
		case MediaCmd::Play:
			if (m_player.Queue().empty()) {
				OnPlayPause();
			} else {
				m_player.Play();
			}
			break;
		case MediaCmd::Pause: m_player.Pause(); break;
		case MediaCmd::Next: m_player.Next(); break;
		case MediaCmd::Prev: m_player.Prev(); break;
	}
}

LRESULT CMainWnd::OnAppCommand(WPARAM, LPARAM lParam)
{
	switch (GET_APPCOMMAND_LPARAM(lParam)) {
		case APPCOMMAND_MEDIA_PLAY_PAUSE: MediaCommand(MediaCmd::PlayPause, false); return TRUE;
		case APPCOMMAND_MEDIA_PLAY: MediaCommand(MediaCmd::Play, false); return TRUE;
		case APPCOMMAND_MEDIA_PAUSE:
		case APPCOMMAND_MEDIA_STOP: MediaCommand(MediaCmd::Pause, false); return TRUE;
		case APPCOMMAND_MEDIA_NEXTTRACK: MediaCommand(MediaCmd::Next, false); return TRUE;
		case APPCOMMAND_MEDIA_PREVIOUSTRACK: MediaCommand(MediaCmd::Prev, false); return TRUE;
	}
	return Default();
}

LRESULT CMainWnd::OnMediaButton(WPARAM wParam, LPARAM)
{
	switch (static_cast<CMediaSession::Button>(wParam)) {
		case CMediaSession::Button::Play: MediaCommand(MediaCmd::Play, true); break;
		case CMediaSession::Button::Pause:
		case CMediaSession::Button::Stop: MediaCommand(MediaCmd::Pause, true); break;
		case CMediaSession::Button::Next: MediaCommand(MediaCmd::Next, true); break;
		case CMediaSession::Button::Previous: MediaCommand(MediaCmd::Prev, true); break;
	}
	return 0;
}

LRESULT CMainWnd::OnPlayerEvent(WPARAM wParam, LPARAM lParam)
{
	return m_player.HandleEvent(wParam, lParam);
}

void CMainWnd::OnTrackStarted(int index, bool resumed)
{
	if (index < 0 || index >= static_cast<int>(m_player.Queue().size())) {
		return;
	}
	const QueueItem item = m_player.Queue()[index]; // copy: the dialog may append
	m_dlg.OnPlaybackStarted(item, resumed);
	UpdateTitle();
	UpdateMediaTrack();
	RefreshQueueList();
	Invalidate(FALSE);
	KHRadioData::SaveQueue(m_player.Queue(), m_player.CurrentIndex(), resumed ? m_player.Position() : 0.0);
}

void CMainWnd::OnNeedResolve(int)
{
	m_dlg.OnNeedResolve();
}

void CMainWnd::UpdateMediaTrack()
{
	const QueueItem* cur = m_player.Current();
	if (!cur) {
		m_media.ClearTrack();
		return;
	}
	const CStringW cover = m_cover.IsLoaded() ? m_cover.Path() : (FileHasData(cur->ref.coverPath) ? cur->ref.coverPath : CStringW());
	m_media.SetTrack(cur->ref.name, cur->ref.albumTitle, cover);
}

void CMainWnd::OnQueueEnded()
{
	if (!m_dlg.ContinueRadioAtEnd()) {
		KHLog::Line(L"queue ended - radio not active, stopping");
	}
	UpdateTitle();
	Invalidate(FALSE);
}

void CMainWnd::OnPlayerError(int index, const CStringW& message)
{
	if (index < 0) {
		m_dlg.SetStatus(message);
	} else {
		m_dlg.SetStatus(L"Skipped a track that would not play: " + message);
	}
}

void CMainWnd::OnQueueChanged()
{
	RefreshQueueList();
	Invalidate(FALSE);
}

void CMainWnd::OnStateChanged()
{
	const auto state = m_player.GetState();
	m_play.SetGlyph(state == CRadioPlayer::State::Playing || state == CRadioPlayer::State::Loading ? kGlyphPause : kGlyphPlay);
	switch (state) {
		case CRadioPlayer::State::Playing: m_media.SetStatus(CMediaSession::Status::Playing); break;
		case CRadioPlayer::State::Loading: m_media.SetStatus(CMediaSession::Status::Changing); break;
		case CRadioPlayer::State::Paused: m_media.SetStatus(CMediaSession::Status::Paused); break;
		case CRadioPlayer::State::Stopped: m_media.SetStatus(CMediaSession::Status::Stopped); break;
	}
}

void CMainWnd::RefreshQueueList()
{
	if (!m_queue.m_hWnd) {
		return;
	}
	// Only the changed tail is rewritten, so the user's scroll position and
	// selection survive the queue growing while they look at it.
	const auto& queue = m_player.Queue();
	const int current = m_player.CurrentIndex();
	size_t same = 0;
	while (same < queue.size() && same < m_listedIds.size() && queue[same].id == m_listedIds[same]) {
		same++;
	}
	if (same != queue.size() || same != m_listedIds.size()) {
		const int top = m_queue.GetTopIndex();
		const int sel = m_queue.GetCurSel();
		const UINT64 selId = (sel >= 0 && sel < static_cast<int>(m_listedIds.size())) ? m_listedIds[static_cast<size_t>(sel)] : 0;
		m_queue.SetRedraw(FALSE);
		for (int i = m_queue.GetCount() - 1; i >= static_cast<int>(same); i--) {
			m_queue.DeleteString(static_cast<UINT>(i));
		}
		m_listedIds.resize(same);
		for (size_t i = same; i < queue.size(); i++) {
			m_queue.AddString(QueueText(queue[i]));
			m_listedIds.push_back(queue[i].id);
		}
		if (selId) {
			m_queue.SetCurSel(m_player.FindById(selId));
		}
		if (top >= 0 && top < m_queue.GetCount()) {
			m_queue.SetTopIndex(top);
		}
		m_queue.SetRedraw(TRUE);
	}
	if (current != m_listedCurrent && current >= 0) {
		// a new track: scroll so it shows near the top
		m_queue.SetTopIndex(std::max(0, current - 1));
	}
	m_listedCurrent = current;
	m_queue.Invalidate(FALSE);
}

void CMainWnd::UpdateTitle()
{
	const QueueItem* cur = m_player.Current();
	SetWindowTextW(cur && m_player.IsActive() ? cur->ref.name + L" - KH Radio" : CStringW(L"KH Radio"));
}

void CMainWnd::ShowAlbumCover(const CStringW& albumUrl, const CStringW& coverPath)
{
	const QueueItem* cur = m_player.Current();
	if (!cur || cur->ref.albumUrl.CompareNoCase(albumUrl) != 0) {
		return; // a cover for an album that is not playing (any more)
	}
	if (coverPath.IsEmpty()) {
		m_cover.Reset();
	} else if (coverPath.CompareNoCase(m_cover.Path()) != 0) {
		if (!m_cover.Load(coverPath)) {
			KHLog::Format(L"cover: could not decode %s", coverPath.GetString());
		}
	}
	if (m_coverScaled) {
		DeleteObject(m_coverScaled);
		m_coverScaled = nullptr;
	}
	InvalidateRect(m_rcCover, FALSE);
	UpdateIconic();
	UpdateMediaTrack();
	PruneCovers(false);
}

void CMainWnd::PruneCovers(bool startup)
{
	// Delete cover files nothing in the queue uses. During a session only files
	// older than 10 minutes go, so a cover just downloaded for an album still
	// being queued is kept.
	std::set<CStringW> keep;
	for (const auto& item : m_player.Queue()) {
		if (!item.ref.coverPath.IsEmpty()) {
			CStringW k = item.ref.coverPath;
			keep.insert(k.MakeLower());
		}
	}
	if (m_cover.IsLoaded()) {
		CStringW k = m_cover.Path();
		keep.insert(k.MakeLower());
	}
	const CStringW dir = KHRadioData::CoverDir();
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	const ULONGLONG nowTicks = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
	constexpr ULONGLONG tenMinutes = 10ull * 60 * 10000000;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(dir + L"cover_*", &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			continue;
		}
		CStringW path = dir + fd.cFileName;
		CStringW lower = path;
		lower.MakeLower();
		if (keep.count(lower)) {
			continue;
		}
		const ULONGLONG written = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
		if (startup || nowTicks - written > tenMinutes) {
			DeleteFileW(path);
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

void CMainWnd::UpdateIconic()
{
	// Taskbar thumbnail shows the cover while one is loaded.
	const BOOL on = m_cover.IsLoaded();
	DwmSetWindowAttribute(m_hWnd, DWMWA_FORCE_ICONIC_REPRESENTATION, &on, sizeof(on));
	DwmSetWindowAttribute(m_hWnd, DWMWA_HAS_ICONIC_BITMAP, &on, sizeof(on));
	if (on) {
		DwmInvalidateIconicBitmaps(m_hWnd);
	}
}

LRESULT CMainWnd::OnIconicThumbnail(WPARAM, LPARAM lParam)
{
	const SIZE maxSize = { HIWORD(lParam), LOWORD(lParam) };
	SIZE actual = {};
	if (HBITMAP bmp = m_cover.CreateScaled(maxSize, &actual)) {
		DwmSetIconicThumbnail(m_hWnd, bmp, 0);
		DeleteObject(bmp);
	}
	return 0;
}

LRESULT CMainWnd::OnIconicLivePreview(WPARAM, LPARAM)
{
	// Peek shows the window as it is.
	CRect client;
	GetClientRect(client);
	if (client.IsRectEmpty()) {
		return 0;
	}
	void* bits = nullptr;
	HBITMAP bmp = CreateDib32(client.Width(), client.Height(), &bits);
	if (!bmp) {
		return 0;
	}
	CClientDC screen(this);
	CDC mem;
	mem.CreateCompatibleDC(&screen);
	HGDIOBJ old = SelectObject(mem.m_hDC, bmp);
	::PrintWindow(m_hWnd, mem.m_hDC, PW_CLIENTONLY | 0x2 /*PW_RENDERFULLCONTENT*/);
	SelectObject(mem.m_hDC, old);
	GdiFlush();
	ForceOpaque(bits, client.Width(), client.Height());
	CRect window;
	GetWindowRect(window);
	CPoint origin(0, 0);
	ClientToScreen(&origin);
	POINT offset = { origin.x - window.left, origin.y - window.top };
	DwmSetIconicLivePreviewBitmap(m_hWnd, bmp, &offset, 0);
	DeleteObject(bmp);
	return 0;
}

void CMainWnd::SaveState()
{
	if (m_stateSaved) {
		return;
	}
	m_stateSaved = true;
	auto& s = KHRadioData::Get();
	WINDOWPLACEMENT wp = { sizeof(wp) };
	if (GetWindowPlacement(&wp)) {
		s.normalRect = wp.rcNormalPosition;
		s.maximized = wp.showCmd == SW_SHOWMAXIMIZED || (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
		s.hasPlacement = true;
	}
	s.volume = m_player.Volume();
	KHRadioData::Save();
	KHRadioData::SaveQueue(m_player.Queue(), m_player.CurrentIndex(), m_player.Position());
}

void CMainWnd::OnClose()
{
	SaveState();
	CWnd::OnClose();
}

void CMainWnd::OnDestroy()
{
	SaveState();
	KillTimer(kUiTimer);
	m_media.Shutdown();
	m_player.Shutdown();
	if (m_coverScaled) {
		DeleteObject(m_coverScaled);
		m_coverScaled = nullptr;
	}
	CWnd::OnDestroy();
}
