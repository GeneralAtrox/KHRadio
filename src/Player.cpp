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
#include "AudioCache.h"
#include "KHParse.h"
#include "Log.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr ULONGLONG kLoadTimeoutMs = 30000;   // no audio this long after opening -> skip
	constexpr ULONGLONG kStallTimeoutMs = 60000;  // playing but no progress this long -> skip
	constexpr int kMaxErrorsInRow = 10;           // stop instead of skipping forever

	// Forwards engine events (worker thread) to the UI thread, tagged with the
	// load they belong to: wParam = event | (load generation << 16).
	class EngineNotify : public IMFMediaEngineNotify
	{
	public:
		EngineNotify(HWND hwnd, std::shared_ptr<std::atomic<UINT>> gen) : m_hwnd(hwnd), m_gen(std::move(gen)) {}

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

		STDMETHODIMP EventNotify(DWORD event, DWORD_PTR, DWORD) override
		{
			switch (event) {
				case MF_MEDIA_ENGINE_EVENT_PLAYING:
				case MF_MEDIA_ENGINE_EVENT_ENDED:
				case MF_MEDIA_ENGINE_EVENT_ERROR:
				case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: {
					const WPARAM packed = (static_cast<WPARAM>(m_gen->load()) << 16) | (event & 0xFFFF);
					PostMessageW(m_hwnd, WM_KHRADIO_PLAYER_EVENT, packed, 0);
					break;
				}
			}
			return S_OK;
		}

	private:
		virtual ~EngineNotify() = default;
		LONG m_ref = 1;
		HWND m_hwnd;
		std::shared_ptr<std::atomic<UINT>> m_gen;
	};

	CStringW EngineErrorText(USHORT code, HRESULT hr)
	{
		LPCWSTR what = L"unknown error";
		switch (code) {
			case MF_MEDIA_ENGINE_ERR_ABORTED: what = L"aborted"; break;
			case MF_MEDIA_ENGINE_ERR_NETWORK: what = L"network error"; break;
			case MF_MEDIA_ENGINE_ERR_DECODE: what = L"decode error"; break;
			case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED: what = L"format not supported (no decoder installed?)"; break;
			case MF_MEDIA_ENGINE_ERR_ENCRYPTED: what = L"encrypted"; break;
		}
		CStringW text;
		text.Format(L"%s (0x%08lX)", what, static_cast<unsigned long>(hr));
		return text;
	}

	// Ogg is decoded to a WAV in the app's cache before it plays; everything
	// else streams from its address.
	QueueItem::Prepare PrepareFor(const CStringW& audioUrl)
	{
		return KHInsider::AudioFormatOf(audioUrl) == KHInsider::AudioFormat::Ogg ? QueueItem::Prepare::DecodeOgg : QueueItem::Prepare::None;
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
		KHLog::Line(L"player: MFStartup failed");
		return false;
	}
	m_mfStarted = true;

	ComPtr<IMFMediaEngineClassFactory> factory;
	ComPtr<IMFAttributes> attributes;
	EngineNotify* notify = new EngineNotify(notifyWnd, m_loadGen);
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
		KHLog::Format(L"player: could not create the Media Foundation engine (0x%08lX)", static_cast<unsigned long>(hr));
		m_engine.Reset();
		return false;
	}
	m_engine->SetVolume(m_volume);
	if (FAILED(m_engine.As(&m_engineEx))) {
		KHLog::Line(L"player: IMFMediaEngineEx is missing - tracks from the audio cache can't play");
	}
	return true;
}

void CRadioPlayer::Shutdown()
{
	++(*m_loadGen);
	m_engineEx.Reset();
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
	return (m_index >= 0 && m_index < static_cast<int>(m_queue.size())) ? &m_queue[static_cast<size_t>(m_index)] : nullptr;
}

QueueItem* CRadioPlayer::Item(int index)
{
	return (index >= 0 && index < static_cast<int>(m_queue.size())) ? &m_queue[static_cast<size_t>(index)] : nullptr;
}

int CRadioPlayer::FindById(UINT64 id) const
{
	for (size_t i = 0; i < m_queue.size(); i++) {
		if (m_queue[i].id == id) {
			return static_cast<int>(i);
		}
	}
	return -1;
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

void CRadioPlayer::NotifyQueue()
{
	if (m_listener) {
		m_listener->OnQueueChanged();
	}
}

void CRadioPlayer::Clear()
{
	++(*m_loadGen);
	if (m_engine) {
		m_engine->Pause();
	}
	m_queue.clear();
	m_index = -1;
	m_started = false;
	m_waiting = false;
	m_sourceSet = false;
	SetState(State::Stopped);
	NotifyQueue();
}

int CRadioPlayer::Append(const QueueItem& item)
{
	// keep the queue bounded over a long session: drop most of the played part
	if (m_index > 300) {
		m_queue.erase(m_queue.begin(), m_queue.begin() + 250);
		m_index -= 250;
	}
	m_queue.push_back(item);
	QueueItem& added = m_queue.back();
	if (!added.id) {
		added.id = NewQueueItemId();
	}
	// a local file only ever comes from Prepared(), never with an appended item
	added.localPath.Empty();
	added.triedLocal = false;
	added.prepare = PrepareFor(added.audioUrl);
	NotifyQueue();
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

void CRadioPlayer::Resolve(int index, const CStringW& audioUrl)
{
	QueueItem* item = Item(index);
	if (!item || audioUrl.IsEmpty()) {
		return;
	}
	item->audioUrl = audioUrl;
	item->prepare = PrepareFor(audioUrl);
	item->localPath.Empty();
	if (index == m_index && m_waiting) {
		if (m_state == State::Paused) {
			m_waiting = false; // starts when the user presses play
		} else {
			Load(index, m_pendingSeek, m_resumed);
		}
	}
}

void CRadioPlayer::Prepared(int index, const CStringW& localPath)
{
	QueueItem* item = Item(index);
	if (!item || !item->IsResolved()) {
		return;
	}
	if (localPath.IsEmpty()) {
		item->prepare = QueueItem::Prepare::None; // play the address itself
	} else if (!KHAudioCache::IsCacheFile(localPath)) {
		KHLog::Format(L"player: refused a local file outside the audio cache: %s", localPath.GetString());
		return;
	}
	item->localPath = localPath;
	if (index == m_index && m_waiting) {
		if (m_state == State::Paused) {
			m_waiting = false; // starts when the user presses play
		} else {
			Load(index, m_pendingSeek, m_resumed);
		}
	}
}

void CRadioPlayer::Remove(int index)
{
	if (index < 0 || index >= static_cast<int>(m_queue.size())) {
		return;
	}
	m_queue.erase(m_queue.begin() + index);
	if (index < m_index) {
		m_index--;
		NotifyQueue();
		return;
	}
	if (index > m_index) {
		NotifyQueue();
		return;
	}
	// the current item went away: move on to what is now at its place
	++(*m_loadGen);
	m_started = false;
	m_sourceSet = false;
	m_waiting = false;
	if (m_engine) {
		m_engine->Pause();
	}
	const bool wasRunning = m_state == State::Loading || m_state == State::Playing;
	if (m_index >= static_cast<int>(m_queue.size())) {
		m_index = static_cast<int>(m_queue.size()) - 1;
		NotifyQueue();
		SetState(State::Stopped);
		if (wasRunning && m_listener) {
			m_listener->OnQueueEnded();
		}
		return;
	}
	if (wasRunning) {
		Load(m_index, 0.0, false);
	} else {
		NotifyQueue();
	}
}

void CRadioPlayer::PlayIndex(int index, double startSec, bool resumed)
{
	m_errorsInRow = 0; // a user's choice starts a fresh count
	Load(index, startSec, resumed);
}

void CRadioPlayer::Load(int index, double startSec, bool resumed)
{
	if (!m_engine || index < 0 || index >= static_cast<int>(m_queue.size())) {
		return;
	}
	++(*m_loadGen); // events still queued for the previous track are ignored
	m_index = index;
	m_started = false;
	m_sourceSet = false;
	m_resumed = resumed;
	m_pendingSeek = startSec > 1.0 ? startSec : 0.0;
	m_lastPos = -1.0;

	QueueItem& item = m_queue[static_cast<size_t>(index)];
	if (!item.localPath.IsEmpty() && GetFileAttributesW(item.localPath) == INVALID_FILE_ATTRIBUTES) {
		item.localPath.Empty(); // the cache let it go (an earlier track): prepare it again
	}
	if (!item.IsResolved() || item.NeedsPrepare()) {
		// no audio address yet, or its file is still being prepared
		m_engine->Pause();
		m_waiting = true;
		SetState(State::Loading);
		NotifyQueue();
		if (m_listener) {
			m_listener->OnNeedResolve(index);
		}
		return;
	}
	m_waiting = false;
	m_loadTick = GetTickCount64();
	m_lastProgressTick = m_loadTick;
	NotifyQueue();

	HRESULT hr = S_OK;
	if (!item.localPath.IsEmpty()) {
		// Only files the app prepared in its own cache folder; queue.json and
		// scraped pages can't name a local file (localPath is never saved).
		if (!KHAudioCache::IsCacheFile(item.localPath)) {
			Fail(L"the prepared file is not in KH Radio's audio cache");
			return;
		}
		hr = OpenLocal(item.localPath);
	} else {
		if (!KHInsider::IsAllowedMediaUrl(item.audioUrl)) {
			Fail(L"the audio address is not on KHInsider's servers");
			return;
		}
		BSTR url = SysAllocString(item.audioUrl);
		hr = url ? m_engine->SetSource(url) : E_OUTOFMEMORY;
		SysFreeString(url);
	}
	if (FAILED(hr)) {
		CStringW msg;
		msg.Format(L"could not open (0x%08lX)", static_cast<unsigned long>(hr));
		Fail(msg);
		return;
	}
	m_sourceSet = true;
	SetState(State::Loading);
	m_engine->Play();
}

HRESULT CRadioPlayer::OpenLocal(const CStringW& path)
{
	if (!m_engineEx) {
		return E_NOINTERFACE;
	}
	ComPtr<IMFByteStream> stream;
	HRESULT hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, path, &stream);
	if (FAILED(hr)) {
		return hr;
	}
	BSTR hint = SysAllocString(path); // its extension (.wav / .m4a) picks the format
	hr = hint ? m_engineEx->SetSourceFromByteStream(stream.Get(), hint) : E_OUTOFMEMORY;
	SysFreeString(hint);
	return hr;
}

bool CRadioPlayer::TryLocalCopy(const CStringW& why)
{
	QueueItem* item = Item(m_index);
	if (!item || item->prepare != QueueItem::Prepare::None || item->triedLocal || !item->localPath.IsEmpty()
			|| KHInsider::AudioFormatOf(item->audioUrl) != KHInsider::AudioFormat::M4a) {
		return false;
	}
	// A progressive m4a can fail to stream (e.g. its index at the end of a file
	// the server won't serve in ranges): fetch the whole file and play that.
	item->triedLocal = true;
	item->prepare = QueueItem::Prepare::Download;
	KHLog::Format(L"player: the m4a stream failed (%s) - downloading it to play from the cache", why.GetString());
	const double at = m_started ? Position() : m_pendingSeek;
	if (m_engine) {
		m_engine->Pause();
	}
	Load(m_index, at, m_resumed);
	return true;
}

void CRadioPlayer::Play()
{
	if (!m_engine) {
		return;
	}
	if (m_state == State::Paused) {
		m_errorsInRow = 0;
		if (!m_sourceSet) {
			Load(m_index, m_pendingSeek, m_resumed); // never opened (or waiting): open it now
			return;
		}
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
	m_errorsInRow = 0;
	if (m_index + 1 < static_cast<int>(m_queue.size())) {
		Load(m_index + 1, 0.0, false);
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
	m_errorsInRow = 0;
	if (Position() > 3.0 || m_index <= 0) {
		Seek(0.0);
	} else {
		Load(m_index - 1, 0.0, false);
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
	if (!m_engine || m_index < 0 || !m_sourceSet) {
		return 0.0;
	}
	const double t = m_engine->GetCurrentTime();
	return std::isfinite(t) && t > 0.0 ? t : 0.0;
}

double CRadioPlayer::Duration() const
{
	if (!m_engine || m_index < 0 || !m_sourceSet) {
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
		Load(m_index + 1, 0.0, false);
	} else {
		++(*m_loadGen);
		SetState(State::Stopped);
		if (m_listener) {
			m_listener->OnQueueEnded();
		}
	}
}

void CRadioPlayer::Fail(const CStringW& message)
{
	const int failed = m_index;
	KHLog::Format(L"player: skipping item %d (%s): %s", failed,
		Current() ? Current()->audioUrl.GetString() : L"?", message.GetString());
	if (m_listener) {
		m_listener->OnPlayerError(failed, message);
	}
	if (m_engine) {
		m_engine->Pause();
	}
	if (++m_errorsInRow >= kMaxErrorsInRow) {
		KHLog::Line(L"player: too many failures in a row - stopping");
		++(*m_loadGen);
		Stop();
		if (m_listener) {
			m_listener->OnPlayerError(-1, L"Too many tracks failed in a row - playback stopped. Press play to try again.");
		}
		return;
	}
	Advance();
}

LRESULT CRadioPlayer::HandleEvent(WPARAM wParam, LPARAM)
{
	if (!m_engine || m_index < 0) {
		return 0;
	}
	const UINT gen = static_cast<UINT>(wParam >> 16);
	if (gen != (m_loadGen->load() & 0xFFFFFFFFu) || !m_sourceSet) {
		return 0; // from a track that was already left
	}
	const DWORD event = static_cast<DWORD>(wParam & 0xFFFF);
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
				Advance();
			}
			break;
		case MF_MEDIA_ENGINE_EVENT_ERROR: {
			ComPtr<IMFMediaError> error;
			if (FAILED(m_engine->GetError(&error)) || !error) {
				break;
			}
			const USHORT code = error->GetErrorCode();
			const HRESULT hr = error->GetExtendedErrorCode();
			if (code == MF_MEDIA_ENGINE_ERR_NOERROR || code == MF_MEDIA_ENGINE_ERR_ABORTED) {
				break;
			}
			const CStringW text = EngineErrorText(code, hr);
			if (!TryLocalCopy(text)) {
				Fail(text);
			}
			break;
		}
	}
	return 0;
}

void CRadioPlayer::Tick()
{
	if (!m_engine || m_index < 0 || m_waiting) {
		return; // waiting for an audio URL: the radio dialog watches that
	}
	const ULONGLONG now = GetTickCount64();
	if (m_state == State::Loading && now - m_loadTick > kLoadTimeoutMs) {
		if (!TryLocalCopy(L"timed out opening it")) {
			Fail(L"timed out opening the track");
		}
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
