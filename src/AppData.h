// KH Radio - data locations, settings.json, queue.json and first-run carry-over.
// GPL-3.0-or-later (see COPYING.txt).
#pragma once
#include <atlstr.h>
#include <set>
#include <vector>
#include "RadioTypes.h"

namespace KHRadioData
{
	// %APPDATA%\KHRadio\ (trailing backslash), created on first use: settings,
	// history, queue, debug log.
	CStringW AppDataDir();
	// %LOCALAPPDATA%\KHRadio\ (trailing backslash): caches and the browser profile.
	CStringW LocalDataDir();
	// %LOCALAPPDATA%\KHRadio\covers\ (trailing backslash), created on first use.
	CStringW CoverDir();
	// %LOCALAPPDATA%\KHRadio\WebView2 (no trailing backslash).
	CStringW WebViewProfileDir();
	// %LOCALAPPDATA%\KHRadio\cache\ (trailing backslash), created on first use:
	// Ogg tracks decoded to WAV, fallback m4a downloads. Emptied at start and exit.
	CStringW CacheDir();
	// %LOCALAPPDATA%\KHRadio\debug\ - pages saved when saveDebugPages is on.
	CStringW DebugDir();
	CStringW HistoryPath();
	CStringW LogPath();

	struct Settings {
		CStringW types;      // csv of the site's form values
		CStringW years;
		CStringW platforms;
		bool avoidPlayed = true;
		bool filterSpoken = false;
		// Tracks shorter than this many seconds are skipped while the filter is on
		// (settings.json only; 0 turns it off). An unknown length is never short.
		int minTrackSeconds = 10;
		bool exclusive = false;
		double volume = 0.8;
		bool hasPlacement = false;
		RECT normalRect = {};
		bool maximized = false;
		bool saveDebugPages = false; // keep the last album/track page in DebugDir()
		// Hidden (settings.json only): "ogg", "m4a", "mp3" or "flac" - take that
		// format first when a track page offers it. Empty: mp3 first, as always.
		CStringW preferAudioFormat;

		bool carryOverDone = false;  // the first-run copy from the MPC-BE fork has run
	};

	enum class LoadResult {
		Missing,    // no settings.json yet (first run)
		Loaded,
		Unreadable, // it could not be read or parsed: set aside (or, failing that, never overwritten)
	};

	Settings& Get();
	LoadResult Load();
	bool Save();

	// First-run copies from the MPC-BE fork: its KHRadio* settings (read-only
	// from HKCU\Software\MPC-BE\Settings), its history and its browser profile.
	// Runs once, only on a real first run (no settings.json), and never moves or
	// deletes the fork's data. Afterwards settings.json records carryOverDone.
	void RunCarryOver(LoadResult settingsResult);

	// Moves album covers from the 1.0 location (%APPDATA%\KHRadio\covers) to CoverDir().
	void MigrateCoverCache();

	struct SavedQueue {
		std::vector<QueueItem> items;
		int index = -1;
		double position = 0.0;
	};
	SavedQueue LoadQueue();
	bool SaveQueue(const std::vector<QueueItem>& items, int index, double position);
	// The same with a chosen file (tests).
	SavedQueue LoadQueue(const CStringW& path);
	bool SaveQueue(const CStringW& path, const std::vector<QueueItem>& items, int index, double position);

	// Tracks the user chose "Play anyway" on: the talking-track filter leaves them
	// alone from then on. Keyed by the track page address in lower case.
	using KeepList = std::set<CStringW>;
	CStringW KeepListPath(); // %APPDATA%\KHRadio\keep_tracks.json
	CStringW KeepKey(const CStringW& pageUrl);
	KeepList LoadKeepList(const CStringW& path);
	bool SaveKeepList(const CStringW& path, const KeepList& keep);
	// The app's own list (UI thread): read from KeepListPath() on first use.
	const KeepList& Kept();
	bool IsKept(const CStringW& pageUrl);
	void AddKept(const CStringW& pageUrl); // and saves the file
}
