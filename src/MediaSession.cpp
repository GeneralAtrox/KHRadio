// KH Radio - Windows media controls (System Media Transport Controls).
// GPL-3.0-or-later (see COPYING.txt).
#include "stdafx.h"
#include "MediaSession.h"
#include "FileUtil.h"
#include "Log.h"

#pragma warning(push, 3)
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.media.h>
#include <windows.storage.streams.h>
#include <SystemMediaTransportControlsInterop.h>
#include <shcore.h>
#pragma warning(pop)

#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "shcore.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace AWM = ABI::Windows::Media;
namespace AWF = ABI::Windows::Foundation;
namespace AWSS = ABI::Windows::Storage::Streams;

struct CMediaSession::Impl {
	HWND hwnd = nullptr;
	ComPtr<AWM::ISystemMediaTransportControls> smtc;
	ComPtr<AWM::ISystemMediaTransportControlsDisplayUpdater> display;
	EventRegistrationToken token = {};
	bool hasToken = false;
};

CMediaSession::CMediaSession() : m(std::make_unique<Impl>()) {}

CMediaSession::~CMediaSession()
{
	Shutdown();
}

bool CMediaSession::Initialize(HWND hwnd)
{
	m->hwnd = hwnd;
	ComPtr<ISystemMediaTransportControlsInterop> interop;
	HRESULT hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Media_SystemMediaTransportControls).Get(),
		IID_PPV_ARGS(&interop));
	if (SUCCEEDED(hr)) {
		hr = interop->GetForWindow(hwnd, IID_PPV_ARGS(&m->smtc));
	}
	if (SUCCEEDED(hr)) {
		hr = m->smtc->get_DisplayUpdater(&m->display);
	}
	if (FAILED(hr)) {
		KHLog::Format(L"media controls: not available (0x%08lX) - media keys work only while KH Radio has focus", static_cast<unsigned long>(hr));
		m->smtc.Reset();
		m->display.Reset();
		return false;
	}
	m->smtc->put_IsEnabled(TRUE);
	m->smtc->put_IsPlayEnabled(TRUE);
	m->smtc->put_IsPauseEnabled(TRUE);
	m->smtc->put_IsNextEnabled(TRUE);
	m->smtc->put_IsPreviousEnabled(TRUE);
	m->smtc->put_IsStopEnabled(TRUE);
	m->display->put_Type(AWM::MediaPlaybackType_Music);

	// Called on a system thread: hand the button to the UI thread.
	const HWND target = hwnd;
	auto handler = Callback<AWF::ITypedEventHandler<AWM::SystemMediaTransportControls*, AWM::SystemMediaTransportControlsButtonPressedEventArgs*>>(
		[target](AWM::ISystemMediaTransportControls*, AWM::ISystemMediaTransportControlsButtonPressedEventArgs* args) -> HRESULT {
			AWM::SystemMediaTransportControlsButton button = AWM::SystemMediaTransportControlsButton_Play;
			if (!args || FAILED(args->get_Button(&button))) {
				return S_OK;
			}
			Button b;
			switch (button) {
				case AWM::SystemMediaTransportControlsButton_Play: b = Button::Play; break;
				case AWM::SystemMediaTransportControlsButton_Pause: b = Button::Pause; break;
				case AWM::SystemMediaTransportControlsButton_Next: b = Button::Next; break;
				case AWM::SystemMediaTransportControlsButton_Previous: b = Button::Previous; break;
				case AWM::SystemMediaTransportControlsButton_Stop: b = Button::Stop; break;
				default: return S_OK;
			}
			PostMessageW(target, WM_KHRADIO_SMTC, static_cast<WPARAM>(b), 0);
			return S_OK;
		});
	if (handler && SUCCEEDED(m->smtc->add_ButtonPressed(handler.Get(), &m->token))) {
		m->hasToken = true;
	}
	SetStatus(Status::Stopped);
	KHLog::Line(L"media controls: registered with Windows");
	return true;
}

void CMediaSession::Shutdown()
{
	if (!m->smtc) {
		return;
	}
	if (m->hasToken) {
		m->smtc->remove_ButtonPressed(m->token);
		m->hasToken = false;
	}
	if (m->display) {
		m->display->ClearAll();
		m->display->Update();
	}
	m->smtc->put_PlaybackStatus(AWM::MediaPlaybackStatus_Closed);
	m->smtc->put_IsEnabled(FALSE);
	m->display.Reset();
	m->smtc.Reset();
}

void CMediaSession::SetStatus(Status status)
{
	if (!m->smtc) {
		return;
	}
	AWM::MediaPlaybackStatus s = AWM::MediaPlaybackStatus_Stopped;
	switch (status) {
		case Status::Stopped: s = AWM::MediaPlaybackStatus_Stopped; break;
		case Status::Changing: s = AWM::MediaPlaybackStatus_Changing; break;
		case Status::Playing: s = AWM::MediaPlaybackStatus_Playing; break;
		case Status::Paused: s = AWM::MediaPlaybackStatus_Paused; break;
	}
	m->smtc->put_PlaybackStatus(s);
}

void CMediaSession::SetTrack(const CStringW& title, const CStringW& album, const CStringW& coverPath)
{
	if (!m->display) {
		return;
	}
	m->display->put_Type(AWM::MediaPlaybackType_Music);
	ComPtr<AWM::IMusicDisplayProperties> music;
	if (SUCCEEDED(m->display->get_MusicProperties(&music)) && music) {
		music->put_Title(HStringReference(title.GetString()).Get());
		music->put_Artist(HStringReference(album.GetString()).Get());
		ComPtr<AWM::IMusicDisplayProperties2> music2;
		if (SUCCEEDED(music.As(&music2))) {
			music2->put_AlbumTitle(HStringReference(album.GetString()).Get());
		}
	}

	ComPtr<AWSS::IRandomAccessStreamReference> thumbnail;
	std::vector<char> bytes;
	if (!coverPath.IsEmpty() && KHFile::ReadAll(coverPath, bytes, 16u * 1024 * 1024) == KHFile::ReadResult::Ok && bytes.size() > 1) {
		ComPtr<IStream> stream;
		stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size() - 1)));
		ComPtr<AWSS::IRandomAccessStream> ras;
		ComPtr<AWSS::IRandomAccessStreamReferenceStatics> statics;
		if (stream
				&& SUCCEEDED(CreateRandomAccessStreamOverStream(stream.Get(), BSOS_DEFAULT, IID_PPV_ARGS(&ras)))
				&& SUCCEEDED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference).Get(),
					IID_PPV_ARGS(&statics)))) {
			statics->CreateFromStream(ras.Get(), &thumbnail);
		}
	}
	m->display->put_Thumbnail(thumbnail.Get());
	m->display->Update();
}

void CMediaSession::ClearTrack()
{
	if (m->display) {
		m->display->ClearAll();
		m->display->Update();
	}
}
