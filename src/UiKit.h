// KH Radio - light/dark palette, small custom controls and the WIC cover image.
// GPL-3.0-or-later (see COPYING.txt).
#pragma once

#include <wrl/client.h>

struct IWICBitmap;

namespace KHTheme
{
	struct Palette {
		bool dark = false;
		COLORREF window = 0;      // main window / dialog background
		COLORREF text = 0;
		COLORREF subtext = 0;
		COLORREF listBk = 0;
		COLORREF listText = 0;
		COLORREF selBk = 0;
		COLORREF selText = 0;
		COLORREF accent = 0;
		COLORREF buttonBk = 0;
		COLORREF buttonPressed = 0;
		COLORREF buttonBorder = 0;
		COLORREF disabled = 0;
		COLORREF track = 0;       // empty part of a slider
		COLORREF placeholder = 0; // cover placeholder fill
	};

	void InitDarkModeSupport();     // once, before windows are created
	bool SystemUsesDarkApps();      // Settings > Personalization > Colors > app mode
	const Palette& Current();
	void Refresh();                 // re-read the system mode
	void ApplyTitleBar(HWND hwnd);
	// Scrollbars and list visuals: dark or light Explorer theme.
	void ApplyToScrollable(HWND hwnd);
	// Checkboxes: classic drawing in dark mode so WM_CTLCOLORSTATIC text colour applies.
	void ApplyToCheckbox(HWND hwnd);
}

// Notifies the parent with WM_KHRADIO_SLIDER (wParam = control id, lParam = 1 when
// the drag is released, 0 while dragging). Value is 0..1.
constexpr UINT WM_KHRADIO_SLIDER = WM_APP + 870;

class CSliderBar : public CWnd
{
public:
	BOOL Create(CWnd* parent, UINT id);
	double GetValue() const { return m_value; }
	void SetValue(double value);       // ignored while the user drags
	bool IsDragging() const { return m_dragging; }

protected:
	DECLARE_MESSAGE_MAP()
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC*) { return TRUE; }
	afx_msg void OnLButtonDown(UINT flags, CPoint pt);
	afx_msg void OnMouseMove(UINT flags, CPoint pt);
	afx_msg void OnLButtonUp(UINT flags, CPoint pt);
	afx_msg void OnCaptureChanged(CWnd* wnd);
	afx_msg void OnKeyDown(UINT ch, UINT rep, UINT flags);
	afx_msg UINT OnGetDlgCode() { return DLGC_WANTARROWS; }
	afx_msg void OnSetFocus(CWnd*) { Invalidate(); }
	afx_msg void OnKillFocus(CWnd*) { Invalidate(); }

private:
	double ValueFromPoint(CPoint pt) const;
	void Notify(bool released);
	double m_value = 0.0;
	bool m_dragging = false;
};

// Owner-drawn round button showing one Segoe MDL2 Assets glyph.
class CGlyphButton : public CButton
{
public:
	BOOL Create(CWnd* parent, UINT id, wchar_t glyph, bool primary);
	void SetGlyph(wchar_t glyph);
	void SetGlyphFont(CFont* font) { m_font = font; }

protected:
	void DrawItem(LPDRAWITEMSTRUCT dis) override;

private:
	wchar_t m_glyph = 0;
	bool m_primary = false;
	CFont* m_font = nullptr;
};

// Album cover decoded once with WIC; scaled copies are made per target size.
class CCoverImage
{
public:
	bool Load(const CStringW& path);
	void Reset();
	bool IsLoaded() const { return m_bitmap != nullptr; }
	const CStringW& Path() const { return m_path; }
	// 32bpp premultiplied top-down DIB fitted inside maxSize, aspect kept.
	HBITMAP CreateScaled(SIZE maxSize, SIZE* actual) const;

private:
	Microsoft::WRL::ComPtr<IWICBitmap> m_bitmap;
	CStringW m_path;
};
