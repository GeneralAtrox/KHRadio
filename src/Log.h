// KH Radio - debug log (khradio_debug.log), safe to call from any thread, also during exit.
// GPL-3.0-or-later (see COPYING.txt).
#pragma once
#include <atlstr.h>

namespace KHLog
{
	// Where the log goes; until this is called (and in the tests) nothing is written.
	void SetPath(const CStringW& path);
	// Appends one timestamped line. The file is rotated to "<path>.old" when it
	// passes 4 MB, at any point in a session.
	void Line(const CStringW& text);
	void Format(_Printf_format_string_ LPCWSTR fmt, ...);
}
