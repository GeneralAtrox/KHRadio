// KH Radio - data locations, settings.json, queue.json and first-run carry-over.
// GPL-3.0-or-later (see COPYING.txt).
#pragma once
#include <atlstr.h>
#include <vector>
#include "RadioTypes.h"

namespace KHRadioData
{
	// %APPDATA%\KHRadio\ (trailing backslash), created on first use.
	CStringW AppDataDir();
	// %APPDATA%\KHRadio\covers\ (trailing backslash), created on first use.
	CStringW CoverDir();
	// %LOCALAPPDATA%\KHRadio\WebView2 (no trailing backslash).
	CStringW WebViewProfileDir();

	struct Settings {
		CStringW types;      // csv of the site's form values
		CStringW years;
		CStringW platforms;
		bool avoidPlayed = true;
		bool filterSpoken = false;
		bool exclusive = false;
		double volume = 0.8;
		bool hasPlacement = false;
		RECT normalRect = {};
		bool maximized = false;
	};

	Settings& Get();
	// Loads settings.json; if it does not exist yet, imports the fork's KHRadio*
	// values (read-only) from HKCU\Software\MPC-BE\Settings and writes the file.
	void Load();
	void Save();

	// First-run copies from the MPC-BE fork. Each runs only when the new
	// location is empty and never moves or deletes the old data.
	void CarryOverHistory();
	void CarryOverWebViewProfile();

	struct SavedQueue {
		std::vector<QueueItem> items;
		int index = -1;
		double position = 0.0;
	};
	SavedQueue LoadQueue();
	void SaveQueue(const std::vector<QueueItem>& items, int index, double position);
}
