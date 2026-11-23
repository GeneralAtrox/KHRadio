// KH Radio - queue item types. GPL-3.0-or-later (see COPYING.txt).
#pragma once
#include <atlstr.h>

// Which album a queued track belongs to (several albums share the queue in
// continuous radio).
struct TrackRef {
	CStringW name;
	CStringW albumUrl;
	CStringW albumTitle;
	CStringW coverPath;  // local cover image for this album, if downloaded
	CStringW platforms;  // console(s) the album is tagged with
};

// Tracks are queued as soon as their album page is read; the direct audio URL
// is looked up from the track page only a few tracks ahead of playback.
struct QueueItem {
	UINT64   id = 0;           // unique within this run (see NewQueueItemId)
	CStringW audioUrl;         // direct CDN audio URL; empty until resolved
	CStringW pageUrl;          // the track's page on downloads.khinsider.com
	CStringW label;            // "NN. Track name"
	TrackRef ref;
	int      resolveFailures = 0;

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
};

// UI thread only.
inline UINT64 NewQueueItemId()
{
	static UINT64 next = 0;
	return ++next;
}
