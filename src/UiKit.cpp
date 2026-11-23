// KH Radio - light/dark palette, small custom controls and the WIC cover image.
// GPL-3.0-or-later (see COPYING.txt).
#include "stdafx.h"
#include "UiKit.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <wincodec.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace KHTheme
{
	namespace
	{
		Palette g_palette;
		bool g_initialized = false;

		// uxtheme exports used by Explorer/Notepad for dark scrollbars; looked up
		// by ordinal and skipped if missing.
		using fnSetPreferredAppMode = int(WINAPI*)(int);
		using fnAllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);
		fnAllowDarkModeForWindow g_allowDarkModeForWindow = nullptr;

		Palette Build(bool dark)
		{
			Palette p;
			p.dark = dark;
			if (dark) {
				p.window = RGB(32, 32, 32);
				p.text = RGB(235, 235, 235);
				p.subtext = RGB(170, 170, 170);
				p.listBk = RGB(25, 25, 25);
				p.listText = RGB(215, 215, 215);
				p.selBk = RGB(0, 84, 153);
				p.selText = RGB(255, 255, 255);
				p.accent = RGB(96, 178, 255);
				p.buttonBk = RGB(55, 55, 55);
				p.buttonPressed = RGB(40, 40, 40);
				p.buttonBorder = RGB(80, 80, 80);
				p.disabled = RGB(110, 110, 110);
				p.track = RGB(80, 80, 80);
				p.placeholder = RGB(45, 45, 45);
			} else {
				p.window = GetSysColor(COLOR_3DFACE);
				p.text = GetSysColor(COLOR_WINDOWTEXT);
				p.subtext = RGB(90, 90, 90);
				p.listBk = GetSysColor(COLOR_WINDOW);
				p.listText = GetSysColor(COLOR_WINDOWTEXT);
				p.selBk = GetSysColor(COLOR_HIGHLIGHT);
				p.selText = GetSysColor(COLOR_HIGHLIGHTTEXT);
				p.accent = RGB(0, 103, 192);
				p.buttonBk = RGB(253, 253, 253);
				p.buttonPressed = RGB(225, 225, 225);
				p.buttonBorder = RGB(200, 200, 200);
				p.disabled = GetSysColor(COLOR_GRAYTEXT);
				p.track = RGB(190, 190, 190);
				p.placeholder = RGB(225, 225, 225);
			}
			return p;
		}
	}

	void InitDarkModeSupport()
	{
		if (g_initialized) {
			return;
		}
		g_initialized = true;
		if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
			auto setMode = reinterpret_cast<fnSetPreferredAppMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
			g_allowDarkModeForWindow = reinterpret_cast<fnAllowDarkModeForWindow>(GetProcAddress(ux, MAKEINTRESOURCEA(133)));
			if (setMode) {
				setMode(1); // AllowDark: follow the per-window choice below
			}
		}
		Refresh();
	}

	bool SystemUsesDarkApps()
	{
		HIGHCONTRAST hc = { sizeof(hc) };
		if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON)) {
			return false; // high contrast: use the system colours
		}
		DWORD value = 1, size = sizeof(value);
		if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
				L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
			return value == 0;
		}
		return false;
	}

	const Palette& Current()
	{
		return g_palette;
	}

	void Refresh()
	{
		g_palette = Build(SystemUsesDarkApps());
	}

	void ApplyTitleBar(HWND hwnd)
	{
		const BOOL dark = g_palette.dark;
		if (FAILED(DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark)))) {
			DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark)); // pre-20H1 value
		}
	}

	void ApplyToScrollable(HWND hwnd)
	{
		if (g_allowDarkModeForWindow) {
			g_allowDarkModeForWindow(hwnd, g_palette.dark);
		}
		SetWindowTheme(hwnd, g_palette.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
		SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
	}

	void ApplyToCheckbox(HWND hwnd)
	{
		if (g_palette.dark) {
			SetWindowTheme(hwnd, L"", L"");
		} else {
			SetWindowTheme(hwnd, nullptr, nullptr);
		}
	}
}

// CSliderBar

BEGIN_MESSAGE_MAP(CSliderBar, CWnd)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_LBUTTONDOWN()
	ON_WM_MOUSEMOVE()
	ON_WM_LBUTTONUP()
	ON_WM_CAPTURECHANGED()
	ON_WM_KEYDOWN()
	ON_WM_GETDLGCODE()
	ON_WM_SETFOCUS()
	ON_WM_KILLFOCUS()
END_MESSAGE_MAP()

BOOL CSliderBar::Create(CWnd* parent, UINT id)
{
	const CString cls = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW, LoadCursorW(nullptr, IDC_HAND), nullptr, nullptr);
	return CWnd::Create(cls, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, CRect(0, 0, 10, 10), parent, id);
}

void CSliderBar::SetValue(double value)
{
	if (m_dragging) {
		return;
	}
	value = std::clamp(value, 0.0, 1.0);
	if (value != m_value) {
		m_value = value;
		if (m_hWnd) {
			Invalidate(FALSE);
		}
	}
}

double CSliderBar::ValueFromPoint(CPoint pt) const
{
	CRect r;
	GetClientRect(r);
	const int pad = r.Height() / 2;
	const int width = std::max(1, r.Width() - 2 * pad);
	return std::clamp(static_cast<double>(pt.x - pad) / width, 0.0, 1.0);
}

void CSliderBar::Notify(bool released)
{
	GetParent()->SendMessageW(WM_KHRADIO_SLIDER, GetDlgCtrlID(), released ? 1 : 0);
}

void CSliderBar::OnPaint()
{
	CPaintDC paint(this);
	CRect r;
	GetClientRect(r);
	CDC dc;
	dc.CreateCompatibleDC(&paint);
	CBitmap bmp;
	bmp.CreateCompatibleBitmap(&paint, r.Width(), r.Height());
	CBitmap* oldBmp = dc.SelectObject(&bmp);

	const auto& p = KHTheme::Current();
	dc.FillSolidRect(r, p.window);
	const int pad = r.Height() / 2;
	const int thick = std::max(2, r.Height() / 6);
	const int cy = r.Height() / 2;
	const int x0 = pad, x1 = std::max(pad, r.Width() - pad);
	const int xv = x0 + static_cast<int>((x1 - x0) * m_value + 0.5);

	dc.FillSolidRect(CRect(x0, cy - thick / 2, x1, cy - thick / 2 + thick), p.track);
	dc.FillSolidRect(CRect(x0, cy - thick / 2, xv, cy - thick / 2 + thick), p.accent);

	const int knob = std::max(4, r.Height() / 3);
	CBrush knobBrush(p.accent);
	CPen knobPen(PS_SOLID, 1, GetFocus() == this ? p.text : p.accent);
	CBrush* oldBrush = dc.SelectObject(&knobBrush);
	CPen* oldPen = dc.SelectObject(&knobPen);
	dc.Ellipse(xv - knob, cy - knob, xv + knob + 1, cy + knob + 1);
	dc.SelectObject(oldBrush);
	dc.SelectObject(oldPen);

	paint.BitBlt(0, 0, r.Width(), r.Height(), &dc, 0, 0, SRCCOPY);
	dc.SelectObject(oldBmp);
}

void CSliderBar::OnLButtonDown(UINT, CPoint pt)
{
	SetFocus();
	SetCapture();
	m_dragging = true;
	m_value = ValueFromPoint(pt);
	Invalidate(FALSE);
	Notify(false);
}

void CSliderBar::OnMouseMove(UINT, CPoint pt)
{
	if (m_dragging) {
		m_value = ValueFromPoint(pt);
		Invalidate(FALSE);
		Notify(false);
	}
}

void CSliderBar::OnLButtonUp(UINT, CPoint pt)
{
	if (m_dragging) {
		m_value = ValueFromPoint(pt);
		m_dragging = false;
		ReleaseCapture();
		Invalidate(FALSE);
		Notify(true);
	}
}

void CSliderBar::OnCaptureChanged(CWnd*)
{
	if (m_dragging) {
		m_dragging = false;
		Notify(true);
	}
}

void CSliderBar::OnKeyDown(UINT ch, UINT rep, UINT flags)
{
	double step = 0.0;
	switch (ch) {
		case VK_LEFT: case VK_DOWN: step = -0.05; break;
		case VK_RIGHT: case VK_UP: step = 0.05; break;
		case VK_HOME: step = -1.0; break;
		case VK_END: step = 1.0; break;
		default: CWnd::OnKeyDown(ch, rep, flags); return;
	}
	m_value = std::clamp(m_value + step, 0.0, 1.0);
	Invalidate(FALSE);
	Notify(true);
}

// CGlyphButton

BOOL CGlyphButton::Create(CWnd* parent, UINT id, wchar_t glyph, bool primary)
{
	m_glyph = glyph;
	m_primary = primary;
	return CButton::Create(L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 10, 10), parent, id);
}

void CGlyphButton::SetGlyph(wchar_t glyph)
{
	if (glyph != m_glyph) {
		m_glyph = glyph;
		if (m_hWnd) {
			Invalidate(FALSE);
		}
	}
}

void CGlyphButton::DrawItem(LPDRAWITEMSTRUCT dis)
{
	CDC* dc = CDC::FromHandle(dis->hDC);
	const auto& p = KHTheme::Current();
	CRect r(dis->rcItem);
	dc->FillSolidRect(r, p.window);

	const bool pressed = !!(dis->itemState & ODS_SELECTED);
	const bool focus = !!(dis->itemState & ODS_FOCUS);
	const int d = std::min(r.Width(), r.Height()) - 2;
	CRect circle(CPoint(r.left + (r.Width() - d) / 2, r.top + (r.Height() - d) / 2), CSize(d, d));

	COLORREF fill = m_primary ? p.accent : (pressed ? p.buttonPressed : p.buttonBk);
	if (m_primary && pressed) {
		fill = RGB(GetRValue(fill) * 4 / 5, GetGValue(fill) * 4 / 5, GetBValue(fill) * 4 / 5);
	}
	CBrush brush(fill);
	CPen pen(PS_SOLID, 1, focus ? p.text : (m_primary ? fill : p.buttonBorder));
	CBrush* oldBrush = dc->SelectObject(&brush);
	CPen* oldPen = dc->SelectObject(&pen);
	dc->Ellipse(circle);
	dc->SelectObject(oldBrush);
	dc->SelectObject(oldPen);

	CFont* oldFont = m_font ? dc->SelectObject(m_font) : nullptr;
	dc->SetBkMode(TRANSPARENT);
	dc->SetTextColor(m_primary ? RGB(255, 255, 255) : p.text);
	wchar_t text[2] = { m_glyph, 0 };
	dc->DrawTextW(text, 1, circle, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	if (oldFont) {
		dc->SelectObject(oldFont);
	}
}

// CCoverImage

namespace
{
	ComPtr<IWICImagingFactory> WicFactory()
	{
		static ComPtr<IWICImagingFactory> factory;
		if (!factory) {
			CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
		}
		return factory;
	}
}

void CCoverImage::Reset()
{
	m_bitmap.Reset();
	m_path.Empty();
}

bool CCoverImage::Load(const CStringW& path)
{
	Reset();
	auto factory = WicFactory();
	if (!factory || path.IsEmpty()) {
		return false;
	}
	ComPtr<IWICBitmapDecoder> decoder;
	ComPtr<IWICBitmapFrameDecode> frame;
	ComPtr<IWICFormatConverter> converter;
	if (FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))
			|| FAILED(decoder->GetFrame(0, &frame))
			|| FAILED(factory->CreateFormatConverter(&converter))
			|| FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
		return false;
	}
	UINT w = 0, h = 0;
	converter->GetSize(&w, &h);
	if (!w || !h) {
		return false;
	}
	// full-size scans can be several thousand pixels; keep at most 1600 px in memory
	ComPtr<IWICBitmapSource> source = converter;
	const UINT cap = 1600;
	if (w > cap || h > cap) {
		const double s = static_cast<double>(cap) / std::max(w, h);
		ComPtr<IWICBitmapScaler> scaler;
		if (SUCCEEDED(factory->CreateBitmapScaler(&scaler))
				&& SUCCEEDED(scaler->Initialize(converter.Get(), std::max(1u, static_cast<UINT>(w * s)), std::max(1u, static_cast<UINT>(h * s)), WICBitmapInterpolationModeFant))) {
			source = scaler;
		}
	}
	if (FAILED(factory->CreateBitmapFromSource(source.Get(), WICBitmapCacheOnLoad, &m_bitmap))) {
		m_bitmap.Reset();
		return false;
	}
	m_path = path;
	return true;
}

HBITMAP CCoverImage::CreateScaled(SIZE maxSize, SIZE* actual) const
{
	auto factory = WicFactory();
	if (!m_bitmap || !factory || maxSize.cx <= 0 || maxSize.cy <= 0) {
		return nullptr;
	}
	UINT w = 0, h = 0;
	m_bitmap->GetSize(&w, &h);
	const double s = std::min(static_cast<double>(maxSize.cx) / w, static_cast<double>(maxSize.cy) / h);
	const UINT tw = std::max(1u, static_cast<UINT>(w * s + 0.5));
	const UINT th = std::max(1u, static_cast<UINT>(h * s + 0.5));

	ComPtr<IWICBitmapScaler> scaler;
	if (FAILED(factory->CreateBitmapScaler(&scaler))
			|| FAILED(scaler->Initialize(m_bitmap.Get(), tw, th, WICBitmapInterpolationModeHighQualityCubic))) {
		return nullptr;
	}
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = static_cast<LONG>(tw);
	bi.bmiHeader.biHeight = -static_cast<LONG>(th);
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!dib) {
		return nullptr;
	}
	if (FAILED(scaler->CopyPixels(nullptr, tw * 4, tw * th * 4, static_cast<BYTE*>(bits)))) {
		DeleteObject(dib);
		return nullptr;
	}
	if (actual) {
		*actual = { static_cast<LONG>(tw), static_cast<LONG>(th) };
	}
	return dib;
}
