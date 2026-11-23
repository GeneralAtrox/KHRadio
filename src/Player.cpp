// KH Radio - audio queue player on Media Foundation's IMFMediaEngine (audio only).
// GPL-3.0-or-later (see COPYING.txt).
#include "stdafx.h"

// winbase.h still has a 16-bit era GetCurrentTime() macro that would rename
// IMFMediaEngine::GetCurrentTime.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <mfapi.h>
#include <mfmediaengine.h>
#include <audiosessiontypes.h>
#include <cmath>

#include "Player.h"
#include "KHInsider.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr ULONGLONG kLoadTimeoutMs = 30000;   // no audio this long after opening -> skip
	constexpr ULONGLONG kStallTimeoutMs = 60000;  // playing but no progress this long -> skip
	constexpr int kMaxErrorsInRow = 10;           // stop instead of skipping forever

	// Forwards engine events (worker thread) to the UI thread.
	class EngineNotify : public IMFMediaEngineNotify
	{
	public:
		explicit EngineNotify(HWND hwnd) : m_hwnd(hwnd) {}

		STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
		{
			if (!ppv) {
				return E_POINTER;
			}
			if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
				*ppv = static_cast<IMFMediaEngineNotify*>(this);
				AddRef();
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
		STDMETHODIMP_(ULONG) Release() override
		{
			const ULONG n = InterlockedDecrement(&m_ref);
			if (!n) {
				delete this;
			}
			return n;
		}

		STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override
		{
			switch (event) {
				case MF_MEDIA_ENGINE_EVENT_PLAYING:
				case MF_MEDIA_ENGINE_EVENT_ENDED:
				case MF_MEDIA_ENGINE_EVENT_ERROR:
				case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
				case MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE: {
					const LPARAM packed = static_cast<LPARAM>((static_cast<ULONGLONG>(param2) << 32) | static_cast<DWORD>(param1));
					PostMessageW(m_hwnd, WM_KHRADIO_PLAYER_EVENT, event, packed);
					break;
				}
			}
			return S_OK;
		}

	private:
		~EngineNotify() = default;
		LONG m_ref = 1;
		HWND m_hwnd;
	};

	CStringW EngineErrorText(USHORT code, HRESULT hr)
	{
		LPCWSTR what = L"unknown error";
		switch (code) {
			case MF_MEDIA_ENGINE_ERR_ABORTED: what = L"aborted"; break;
			case MF_MEDIA_ENGINE_ERR_NETWORK: what = L"network error"; break;
			case MF_MEDIA_ENGINE_ERR_DECODE: what = L"decode error"; break;
			case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED: what = L"format not supported (no decoder?)"; break;
			case MF_MEDIA_ENGINE_ERR_ENCRYPTED: what = L"encrypted"; break;
		}
		CStringW text;
		text.Format(L"%s (0x%08lX)", what, static_cast<unsigned long>(hr));
		return text;
	}
}

CRadioPlayer::~CRadioPlayer()
{
	Shutdown();
}

bool CRadioPlayer::Initialize(HWND notifyWnd, IPlayerListener* listener)
{
	m_listener = listener;
	if (FAILED(MFStartup(MF_VERSION))) {
		KHInsider::DebugLog(L"player: MFStartup failed");
		return false;
	}
	m_mfStarted = true;

	ComPtr<IMFMediaEngineClassFactory> factory;
	ComPtr<IMFAttributes> attributes;
	EngineNotify* notify = new EngineNotify(notifyWnd);
	HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
	if (SUCCEEDED(hr)) {
		hr = MFCreateAttributes(&attributes, 2);
	}
	if (SUCCEEDED(hr)) {
		hr = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
	}
	if (SUCCEEDED(hr)) {
		attributes->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, AudioCategory_Media);
		hr = factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY, attributes.Get(), &m_engine);
	}
	notify->Release();
	if (FAILED(hr)) {
		CStringW log;
		log.Format(L"player: could not create the Media Foundation engine (0x%08lX)", static_cast<unsigned long>(hr));
		KHInsider::DebugLog(log);
		m_engine.Reset();
		return false;
	}
	m_engine->SetVolume(m_volume);
	return true;
}

void CRadioPlayer::Shutdown()
{
	if (m_engine) {
		m_engine->Shutdown();
		m_engine.Reset();
	}
	if (m_mfStarted) {
		MFShutdown();
		m_mfStarted = false;
	}
	m_listener = nullptr;
}

const QueueItem* CRadioPlayer::Current() const
{
	return (m_index >= 0 && m_index < static_cast<int>(m_queue.size())) ? &m_queue[m_index] : nullptr;
}

void CRadioPlayer::SetState(State state)
{
	if (m_state != state) {
		m_state = state;
		if (m_listener) {
			m_listener->OnStateChanged();
		}
	}
}

void CRadioPlayer::Clear()
{
	if (m_engine) {
		m_engine->Pause();
	}
	m_queue.clear();
	m_index = -1;
	m_started = false;
	SetState(State::Stopped);
	if (m_listener) {
		m_listener->OnQueueChanged();
	}
}

int CRadioPlayer::Append(const QueueItem& item)
{
	// keep the queue bounded over a long session: drop most of the played part
	if (m_index > 300) {
		m_queue.erase(m_queue.begin(), m_queue.begin() + 250);
		m_index -= 250;
	}
	m_queue.push_back(item);
	if (m_listener) {
		m_listener->OnQueueChanged();
	}
	return static_cast<int>(m_queue.size()) - 1;
}

void CRadioPlayer::UpdateAlbumCover(const CStringW& albumUrl, const CStringW& coverPath)
{
	for (auto& item : m_queue) {
		if (item.ref.albumUrl.CompareNoCase(albumUrl) == 0) {
			item.ref.coverPath = coverPath;
		}
	}
}

void CRadioPlayer::PlayIndex(int index, double startSec, bool resumed)
{
	if (!m_engine || index < 0 || index >= static_cast<int>(m_queue.size())) {
		return;
	}
	m_index = index;
	m_started = false;
	m_resumed = resumed;
	m_pendingSeek = startSec > 1.0 ? startSec : 0.0;
	m_loadTick = GetTickCount64();
	m_lastPos = -1.0;
	m_lastProgressTick = m_loadTick;

	BSTR url = SysAllocString(m_queue[index].audioUrl);
	const HRESULT hr = m_engine->SetSource(url);
	SysFreeString(url);
	if (m_listener) {
		m_listener->OnQueueChanged();
	}
	if (FAILED(hr)) {
		CStringW msg;
		msg.Format(L"could not open (0x%08lX)", static_cast<unsigned long>(hr));
		Fail(msg);
		return;
	}
	SetState(State::Loading);
	m_engine->Play();
}

void CRadioPlayer::Play()
{
	if (!m_engine) {
		return;
	}
	if (m_state == State::Paused) {
		m_engine->Play();
		if (m_started) {
			SetState(State::Playing);
		} else {
			m_loadTick = GetTickCount64();
			SetState(State::Loading);
		}
	} else if (m_state == State::Stopped && !m_queue.empty()) {
		PlayIndex(std::max(0, m_index));
	}
}

void CRadioPlayer::Pause()
{
	if (m_engine && (m_state == State::Playing || m_state == State::Loading)) {
		m_engine->Pause();
		SetState(State::Paused);
	}
}

void CRadioPlayer::TogglePause()
{
	if (m_state == State::Playing || m_state == State::Loading) {
		Pause();
	} else {
		Play();
	}
}

void CRadioPlayer::Next()
{
	if (m_index + 1 < static_cast<int>(m_queue.size())) {
		PlayIndex(m_index + 1);
	} else {
		// at the end: let the radio fetch more instead of stopping silently
		Stop();
		if (m_listener) {
			m_listener->OnQueueEnded();
		}
	}
}

void CRadioPlayer::Prev()
{
	if (Position() > 3.0 || m_index <= 0) {
		Seek(0.0);
	} else {
		PlayIndex(m_index - 1);
	}
}

void CRadioPlayer::Stop()
{
	if (m_engine) {
		m_engine->Pause();
	}
	SetState(State::Stopped);
}

void CRadioPlayer::Seek(double seconds)
{
	if (!m_engine || !m_started) {
		return;
	}
	const double duration = Duration();
	if (duration > 0.0) {
		seconds = std::clamp(seconds, 0.0, std::max(0.0, duration - 0.25));
	}
	m_engine->SetCurrentTime(std::max(0.0, seconds));
	m_lastProgressTick = GetTickCount64();
}

double CRadioPlayer::Position() const
{
	if (!m_engine || m_index < 0) {
		return 0.0;
	}
	const double t = m_engine->GetCurrentTime();
	return std::isfinite(t) && t > 0.0 ? t : 0.0;
}

double CRadioPlayer::Duration() const
{
	if (!m_engine || m_index < 0) {
		return 0.0;
	}
	const double d = m_engine->GetDuration();
	return std::isfinite(d) && d > 0.0 ? d : 0.0;
}

void CRadioPlayer::SetVolume(double volume)
{
	m_volume = std::clamp(volume, 0.0, 1.0);
	if (m_engine) {
		m_engine->SetVolume(m_volume);
	}
}

void CRadioPlayer::Advance()
{
	if (m_index + 1 < static_cast<int>(m_queue.size())) {
		PlayIndex(m_index + 1);
	} else {
		SetState(State::Stopped);
		if (m_listener) {
			m_listener->OnQueueEnded();
		}
	}
}

void CRadioPlayer::Fail(const CStringW& message)
{
	const int failed = m_index;
	CStringW log;
	log.Format(L"player: skipping item %d (%s): %s", failed,
		Current() ? Current()->audioUrl.GetString() : L"?", message.GetString());
	KHInsider::DebugLog(log);
	if (m_listener) {
		m_listener->OnPlayerError(failed, message);
	}
	if (++m_errorsInRow >= kMaxErrorsInRow) {
		KHInsider::DebugLog(L"player: too many failures in a row - stopping");
		Stop();
		if (m_listener) {
			m_listener->OnPlayerError(-1, L"Too many tracks failed in a row - playback stopped.");
		}
		return;
	}
	if (m_engine) {
		m_engine->Pause();
	}
	Advance();
}

LRESULT CRadioPlayer::HandleEvent(WPARAM wParam, LPARAM lParam)
{
	if (!m_engine || m_index < 0) {
		return 0;
	}
	const DWORD event = static_cast<DWORD>(wParam);
	switch (event) {
		case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
			if (m_pendingSeek > 0.0) {
				m_engine->SetCurrentTime(m_pendingSeek);
				m_pendingSeek = 0.0;
			}
			break;
		case MF_MEDIA_ENGINE_EVENT_PLAYING:
			if (m_state == State::Paused || m_state == State::Stopped) {
				break; // a late event for a play the user already paused
			}
			m_errorsInRow = 0;
			m_lastProgressTick = GetTickCount64();
			SetState(State::Playing);
			if (!m_started) {
				m_started = true;
				const bool resumed = m_resumed;
				m_resumed = false;
				if (m_listener) {
					m_listener->OnTrackStarted(m_index, resumed);
				}
			}
			break;
		case MF_MEDIA_ENGINE_EVENT_ENDED:
			if (m_state == State::Playing && m_engine->IsEnded()) {
				if (m_listener) {
					m_listener->OnTrackEnded(m_index);
				}
				Advance();
			}
			break;
		case MF_MEDIA_ENGINE_EVENT_ERROR: {
			// A stale error from the previous source finds no error on the new one.
			ComPtr<IMFMediaError> error;
			if (FAILED(m_engine->GetError(&error)) || !error) {
				break;
			}
			const USHORT code = error->GetErrorCode();
			const HRESULT hr = error->GetExtendedErrorCode();
			if (code == MF_MEDIA_ENGINE_ERR_NOERROR || code == MF_MEDIA_ENGINE_ERR_ABORTED) {
				break;
			}
			UNREFERENCED_PARAMETER(lParam);
			Fail(EngineErrorText(code, hr));
			break;
		}
	}
	return 0;
}

void CRadioPlayer::Tick()
{
	if (!m_engine || m_index < 0) {
		return;
	}
	const ULONGLONG now = GetTickCount64();
	if (m_state == State::Loading && now - m_loadTick > kLoadTimeoutMs) {
		Fail(L"timed out opening the track");
		return;
	}
	if (m_state == State::Playing) {
		const double pos = Position();
		if (pos != m_lastPos) {
			m_lastPos = pos;
			m_lastProgressTick = now;
		} else if (now - m_lastProgressTick > kStallTimeoutMs && !m_engine->IsSeeking()) {
			Fail(L"stream stalled");
		}
	}
}
