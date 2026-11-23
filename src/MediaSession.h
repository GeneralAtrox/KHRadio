// KH Radio - Windows media controls (System Media Transport Controls): the media
// keys, the volume flyout and the lock screen show KH Radio's track and cover and
// can play, pause and skip. GPL-3.0-or-later (see COPYING.txt).
#pragma once
#include <atlstr.h>
#include <memory>

// Posted to the window passed to Initialize: wParam = CMediaSession::Button.
constexpr UINT WM_KHRADIO_SMTC = WM_APP + 890;

class CMediaSession
{
public:
	enum class Button { Play, Pause, Next, Previous, Stop };
	enum class Status { Stopped, Changing, Playing, Paused };

	CMediaSession();
	~CMediaSession();
	CMediaSession(const CMediaSession&) = delete;
	CMediaSession& operator=(const CMediaSession&) = delete;

	// False if Windows has no media controls for this window (they are then simply absent).
	bool Initialize(HWND hwnd);
	void Shutdown();
	void SetStatus(Status status);
	// Title = track, artist and album = album title; the cover is read from coverPath.
	void SetTrack(const CStringW& title, const CStringW& album, const CStringW& coverPath);
	void ClearTrack();

private:
	struct Impl;
	std::unique_ptr<Impl> m;
};
