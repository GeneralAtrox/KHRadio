// KH Radio - queue item types. GPL-3.0-or-later (see COPYING.txt).
#pragma once
#include <atlstr.h>
#include <algorithm>
#include <vector>

// Which album a queued track belongs to (several albums share the queue in
// continuous radio).
struct TrackRef {
	CStringW name;
	CStringW albumUrl;
	CStringW albumTitle;
	CStringW coverPath;  // local cover image for this album, if downloaded
	CStringW platforms;  // console(s) the album is tagged with
};

// Why the talking-track filter passes over a queued track. A skipped track
// stays in the queue as a dimmed row; playback steps over it.
enum class SkipReason : unsigned char {
	None,
	Talking, // the audio sounds like people talking (a drama or cutscene track)
	Short,   // shorter than the minimum length (settings.json "minTrackSeconds")
	Silent,  // every window that was checked is silent
	Title,   // the track title says it is a drama / voice track
};

// Short text for the dimmed queue row and the status line.
inline LPCWSTR SkipReasonText(SkipReason r)
{
	switch (r) {
		case SkipReason::Talking: return L"sounds like talking (drama or cutscene)";
		case SkipReason::Short: return L"too short";
		case SkipReason::Silent: return L"silent";
		case SkipReason::Title: return L"title says drama / voice track";
		default: return L"";
	}
}

// queue.json spelling
inline LPCWSTR SkipReasonKey(SkipReason r)
{
	switch (r) {
		case SkipReason::Talking: return L"talking";
		case SkipReason::Short: return L"short";
		case SkipReason::Silent: return L"silent";
		case SkipReason::Title: return L"title";
		default: return L"";
	}
}

inline SkipReason SkipReasonFromKey(const CStringW& key)
{
	for (const SkipReason r : { SkipReason::Talking, SkipReason::Short, SkipReason::Silent, SkipReason::Title }) {
		if (key.CompareNoCase(SkipReasonKey(r)) == 0) {
			return r;
		}
	}
	return SkipReason::None;
}

// Tracks are queued as soon as their album page is read; the direct audio URL
// is looked up from the track page only a few tracks ahead of playback.
struct QueueItem {
	UINT64   id = 0;           // unique within this run (see NewQueueItemId)
	CStringW audioUrl;         // direct CDN audio URL; empty until resolved
	CStringW pageUrl;          // the track's page on downloads.khinsider.com
	CStringW label;            // "NN. Track name"
	TrackRef ref;
	int      resolveFailures = 0;
	bool     shared = false;   // the track a pasted share link asked for: never filtered out (not saved)
	SkipReason skip = SkipReason::None; // the filter passes over it (saved)
	bool     keep = false;     // "Play anyway": the filter leaves it alone (saved)
	bool     judged = false;   // the audio check has run on it (not saved)

	// How the item is played. Ogg is decoded to a WAV in the app's own cache
	// folder; an m4a whose stream fails is downloaded there as it is. The player
	// sets this from audioUrl. None of these fields is saved to queue.json.
	enum class Prepare : unsigned char { None, DecodeOgg, Download };
	Prepare  prepare = Prepare::None;
	CStringW localPath;        // the prepared file; set only by the app, after preparing it
	bool     triedLocal = false;  // the m4a stream failed once and a local copy was asked for
	int      prepareFailures = 0;

	bool IsResolved() const { return !audioUrl.IsEmpty(); }
	bool NeedsPrepare() const { return prepare != Prepare::None && localPath.IsEmpty(); }
	bool IsSkipped() const { return skip != SkipReason::None; }
};

// Index of the nearest playable (not skipped) item after 'from' (dir > 0) or
// before it (dir < 0); -1 if there is none.
inline int StepPlayable(const std::vector<QueueItem>& queue, int from, int dir)
{
	const int n = static_cast<int>(queue.size());
	for (int i = from + (dir < 0 ? -1 : 1); i >= 0 && i < n; i += (dir < 0 ? -1 : 1)) {
		if (!queue[static_cast<size_t>(i)].IsSkipped()) {
			return i;
		}
	}
	return -1;
}

// 'index' itself if it is playable, else the next playable item after it; -1 if none.
inline int PlayableAtOrAfter(const std::vector<QueueItem>& queue, int index)
{
	if (index < 0) {
		index = 0;
	}
	if (index < static_cast<int>(queue.size()) && !queue[static_cast<size_t>(index)].IsSkipped()) {
		return index;
	}
	return StepPlayable(queue, index, 1);
}

// Last index of the look-ahead window: 'cur' plus the next 'ahead' playable
// items (skipped rows in between don't use up the window).
inline int AheadLimit(const std::vector<QueueItem>& queue, int cur, int ahead)
{
	int last = std::min(cur, static_cast<int>(queue.size()) - 1);
	for (int i = 0; i < ahead; i++) {
		const int next = StepPlayable(queue, last, 1);
		if (next < 0) {
			return static_cast<int>(queue.size()) - 1;
		}
		last = next;
	}
	return last;
}

// Playable items after 'cur' (cur = -1: all of them).
inline int CountPlayableAfter(const std::vector<QueueItem>& queue, int cur)
{
	int n = 0;
	for (size_t i = static_cast<size_t>(std::max(-1, cur) + 1); i < queue.size(); i++) {
		n += queue[i].IsSkipped() ? 0 : 1;
	}
	return n;
}

// UI thread only.
inline UINT64 NewQueueItemId()
{
	static UINT64 next = 0;
	return ++next;
}
