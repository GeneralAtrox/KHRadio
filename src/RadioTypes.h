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

struct QueueItem {
	CStringW audioUrl;   // direct CDN audio URL
	CStringW label;      // "NN. Track name"
	TrackRef ref;
};
