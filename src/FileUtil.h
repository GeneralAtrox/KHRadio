// KH Radio - small file helpers: whole-file reads, atomic writes, setting a bad file aside.
// GPL-3.0-or-later (see COPYING.txt).
#pragma once
#include <atlstr.h>
#include <vector>

namespace KHFile
{
	enum class ReadResult {
		Ok,       // read completely; data holds the bytes plus a trailing NUL
		Missing,  // the file does not exist
		Error,    // it exists but could not be read (locked, too big, I/O error)
	};

	// Reads a whole file (up to maxBytes) and appends a NUL so text parsers can use it.
	ReadResult ReadAll(const CStringW& path, std::vector<char>& data, size_t maxBytes = 64u * 1024 * 1024);

	// Writes to "<path>.tmp", flushes it to disk, then renames it over 'path', so a
	// crash or a full disk never leaves a cut-short file. With keepBackup the old
	// file is first copied to "<path>.bak" (one rolling backup).
	bool WriteAtomic(const CStringW& path, const void* bytes, size_t len, bool keepBackup = false);

	// Renames a file that could not be read or parsed to "<path>.<tag>-YYYYMMDD-HHMMSS"
	// so nothing overwrites it. Returns the new name, or an empty string on failure.
	CStringW SetAside(const CStringW& path, LPCWSTR tag);

	bool Exists(const CStringW& path);
}
