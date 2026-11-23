// KH Radio - the audio cache: tracks the player can't stream (Ogg Vorbis, and an
// m4a whose stream failed) are prepared as local files in one folder the app
// owns, %LOCALAPPDATA%\KHRadio\cache\. Ogg is decoded to 16-bit PCM WAV with
// stb_vorbis, streamed to disk. GPL-3.0-or-later (see COPYING.txt).
#pragma once
#include <atlstr.h>
#include <functional>
#include <vector>

namespace KHAudioCache
{
	// The cache folder (trailing backslash). Set once at startup, before any
	// worker runs; a folder that is a link (reparse point) is refused and the
	// cache stays off (Dir() empty).
	bool SetDir(const CStringW& dir);
	CStringW Dir();

	// True only for a file the cache itself names ("khr_<16 hex>.wav" or ".m4a")
	// directly inside 'dir'. Pure string check, no file access.
	bool IsInside(const CStringW& dir, const CStringW& path);
	// IsInside(Dir(), path): the only local files the player accepts.
	bool IsCacheFile(const CStringW& path);

	// Dir() + "khr_<id>.<ext>" (empty while the cache is off). ext: "wav", "m4a",
	// or a working name such as "ogg.part".
	CStringW PathFor(unsigned long long itemId, LPCWSTR ext);

	// Total size of the cache's files.
	unsigned long long TotalBytes();
	// Deletes every cache file (startup and exit). Files in use stay.
	void ClearAll();
	// Deletes the files of items not in 'keep', then, while the rest is over
	// capBytes, the oldest ones except protectId's. Files in use stay for later.
	void Prune(const std::vector<unsigned long long>& keep, unsigned long long protectId, unsigned long long capBytes);

	enum class DecodeResult {
		Ok,
		NotVorbis,  // not an Ogg Vorbis stream stb_vorbis can read (e.g. Opus in Ogg)
		Failed,     // read/write error, corrupt stream, disk full
		TooBig,     // the WAV would pass maxWavBytes
		Cancelled,
	};
	struct DecodeInfo {
		int channels = 0;            // in the WAV (1 or 2)
		int sampleRate = 0;
		unsigned long long frames = 0;
		std::vector<float> monoPrefix; // first prefixSeconds as mono float, for the speech check
	};
	// Decodes oggPath into wavPath (written as wavPath + ".part", renamed when
	// complete). Nothing is left behind unless it returns Ok.
	DecodeResult OggToWav(const CStringW& oggPath, const CStringW& wavPath, unsigned long long maxWavBytes,
		const std::function<bool()>& cancelled, DecodeInfo* info, double prefixSeconds = 12.0);

	// 44-byte header of a 16-bit PCM WAV.
	void WavHeader(unsigned char out[44], int channels, int sampleRate, unsigned long dataBytes);
}
