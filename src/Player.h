// KH Radio - audio queue player on Media Foundation's IMFMediaEngine (audio only).
// GPL-3.0-or-later (see COPYING.txt).
#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <wrl/client.h>
#include "RadioTypes.h"

// winbase.h still has a 16-bit era GetCurrentTime() macro that would rename
// IMFMediaEngine::GetCurrentTime.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <mfmediaengine.h>

// Posted by the engine's worker thread to the notify window; the window must
// pass it to CRadioPlayer::HandleEvent. wParam carries the event and the load
// it belongs to, so events from a track that was already left are dropped.
constexpr UINT WM_KHRADIO_PLAYER_EVENT = WM_APP + 860;

// All callbacks run on the UI thread.
struct IPlayerListener {
	virtual void OnTrackStarted(int index, bool resumed) = 0; // first audio of a queue item
	virtual void OnQueueEnded() = 0;                          // nothing left after the last item
	virtual void OnPlayerError(int index, const CStringW& message) = 0; // the item is skipped
	virtual void OnQueueChanged() = 0;                        // items or current index changed
	virtual void OnStateChanged() = 0;                        // play/pause/stop
	virtual void OnNeedResolve(int index) = 0;                // the item to play has no audio URL yet
};

class CRadioPlayer
{
public:
	enum class State { Stopped, Loading, Playing, Paused };

	CRadioPlayer() = default;
	~CRadioPlayer();
	CRadioPlayer(const CRadioPlayer&) = delete;
	CRadioPlayer& operator=(const CRadioPlayer&) = delete;

	bool Initialize(HWND notifyWnd, IPlayerListener* listener);
	void Shutdown();
	LRESULT HandleEvent(WPARAM wParam, LPARAM lParam);
	void Tick(); // call a few times a second: load/stall watchdog

	// queue
	void Clear();
	int Append(const QueueItem& item); // returns the new item's index
	const std::vector<QueueItem>& Queue() const { return m_queue; }
	QueueItem* Item(int index);
	int FindById(UINT64 id) const;
	int CurrentIndex() const { return m_index; }
	const QueueItem* Current() const;
	bool IsAtEnd() const { return m_index >= 0 && m_index == static_cast<int>(m_queue.size()) - 1; }
	bool IsWaitingForResolve() const { return m_waiting; }
	void UpdateAlbumCover(const CStringW& albumUrl, const CStringW& coverPath);
	// The audio URL for an item arrived; if it is the one waiting to play, it starts.
	void Resolve(int index, const CStringW& audioUrl);
	// The item's file in the audio cache is ready (it must be a cache file), or,
	// with an empty path, the item plays from its address instead. If it is the
	// one waiting to play, it starts.
	void Prepared(int index, const CStringW& localPath);
	// Drops an item that can't be played; if it was the current one, the next plays.
	void Remove(int index);

	// transport (user actions: each one clears the failed-in-a-row count)
	void PlayIndex(int index, double startSec = 0.0, bool resumed = false);
	void Play();
	void Pause();
	void TogglePause();
	void Next();
	void Prev();
	void Stop();
	void Seek(double seconds);
	double Position() const;
	double Duration() const;
	void SetVolume(double volume);
	double Volume() const { return m_volume; }
	State GetState() const { return m_state; }
	bool IsActive() const { return m_state != State::Stopped; }

private:
	void Load(int index, double startSec, bool resumed);
	HRESULT OpenLocal(const CStringW& path);
	bool TryLocalCopy(const CStringW& why); // a failed m4a stream: play a downloaded copy instead
	void Advance(); // after an end or an error
	void Fail(const CStringW& message);
	void SetState(State state);
	void NotifyQueue();

	Microsoft::WRL::ComPtr<IMFMediaEngine> m_engine;
	Microsoft::WRL::ComPtr<IMFMediaEngineEx> m_engineEx; // opens local files from the audio cache
	IPlayerListener* m_listener = nullptr;
	bool m_mfStarted = false;
	std::shared_ptr<std::atomic<UINT>> m_loadGen = std::make_shared<std::atomic<UINT>>(0);

	std::vector<QueueItem> m_queue;
	int m_index = -1;
	State m_state = State::Stopped;
	bool m_started = false;      // current item reported OnTrackStarted
	bool m_resumed = false;
	bool m_waiting = false;      // current item has no audio URL yet, or its file is being prepared
	bool m_sourceSet = false;    // the engine holds the current item's source
	double m_pendingSeek = 0.0;
	double m_volume = 0.8;
	ULONGLONG m_loadTick = 0;
	double m_lastPos = -1.0;
	ULONGLONG m_lastProgressTick = 0;
	int m_errorsInRow = 0;
};
