// KH Radio - the audio cache (see AudioCache.h). GPL-3.0-or-later (see COPYING.txt).
#include "stdafx.h"
#include "AudioCache.h"
#include "Log.h"

#include <cstdio>
#include <mutex>

// stb_vorbis.c itself is compiled as its own C file (see the project); only its
// declarations are needed here.
#pragma warning(push, 0)
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis/stb_vorbis.c"
#pragma warning(pop)

namespace KHAudioCache
{
	namespace
	{
		SRWLOCK g_lock = SRWLOCK_INIT;
		CStringW g_dir; // empty: the cache is off

		constexpr int kIdDigits = 16;

		bool IsHex(WCHAR c)
		{
			return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
		}

		// "khr_<16 hex>" followed by '.', or -1. Returns the id through 'id'.
		bool ParseName(const CStringW& name, unsigned long long& id)
		{
			if (name.GetLength() < 4 + kIdDigits + 2 || name.Left(4).CompareNoCase(L"khr_") != 0 || name[4 + kIdDigits] != L'.') {
				return false;
			}
			for (int i = 4; i < 4 + kIdDigits; i++) {
				if (!IsHex(name[i])) {
					return false;
				}
			}
			id = wcstoull(name.Mid(4, kIdDigits), nullptr, 16);
			return true;
		}

		struct CacheFile {
			CStringW path;
			unsigned long long id = 0;
			unsigned long long size = 0;
			ULONGLONG written = 0;
		};

		// Plain files of the cache's own naming in the folder; never recurses,
		// never follows links.
		std::vector<CacheFile> ListFiles()
		{
			std::vector<CacheFile> files;
			const CStringW dir = Dir();
			if (dir.IsEmpty()) {
				return files;
			}
			WIN32_FIND_DATAW fd = {};
			HANDLE h = FindFirstFileExW(dir + L"khr_*", FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
			if (h == INVALID_HANDLE_VALUE) {
				return files;
			}
			do {
				if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
					continue;
				}
				CacheFile f;
				if (!ParseName(fd.cFileName, f.id)) {
					continue;
				}
				f.path = dir + fd.cFileName;
				f.size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
				f.written = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
				files.push_back(f);
			} while (FindNextFileW(h, &fd));
			FindClose(h);
			return files;
		}

		bool WriteAll(HANDLE h, const void* data, DWORD len)
		{
			DWORD written = 0;
			return WriteFile(h, data, len, &written, nullptr) && written == len;
		}

		void Put16(unsigned char* p, unsigned v) { p[0] = static_cast<unsigned char>(v); p[1] = static_cast<unsigned char>(v >> 8); }
		void Put32(unsigned char* p, unsigned long v)
		{
			for (int i = 0; i < 4; i++) {
				p[i] = static_cast<unsigned char>(v >> (8 * i));
			}
		}
	}

	bool SetDir(const CStringW& dir)
	{
		CStringW d = dir;
		if (!d.IsEmpty() && d.Right(1) != L"\\") {
			d += L"\\";
		}
		const DWORD attr = d.IsEmpty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(d);
		const bool ok = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT)
			&& d.Find(L"\\..\\") == -1 && d.Find(L'/') == -1;
		if (!ok && !d.IsEmpty()) {
			KHLog::Format(L"cache: %s is missing or is a link - the audio cache is off", d.GetString());
		}
		AcquireSRWLockExclusive(&g_lock);
		g_dir = ok ? d : CStringW();
		ReleaseSRWLockExclusive(&g_lock);
		return ok;
	}

	CStringW Dir()
	{
		AcquireSRWLockShared(&g_lock);
		CStringW d = g_dir;
		ReleaseSRWLockShared(&g_lock);
		return d;
	}

	bool IsInside(const CStringW& dir, const CStringW& path)
	{
		if (dir.GetLength() < 4 || dir.Right(1) != L"\\" || path.GetLength() <= dir.GetLength()
				|| path.Left(dir.GetLength()).CompareNoCase(dir) != 0) {
			return false;
		}
		const CStringW name = path.Mid(dir.GetLength());
		unsigned long long id = 0;
		if (name.FindOneOf(L"\\/:") != -1 || !ParseName(name, id)) {
			return false;
		}
		const CStringW ext = name.Mid(4 + kIdDigits);
		return ext.CompareNoCase(L".wav") == 0 || ext.CompareNoCase(L".m4a") == 0;
	}

	bool IsCacheFile(const CStringW& path)
	{
		return IsInside(Dir(), path);
	}

	CStringW PathFor(unsigned long long itemId, LPCWSTR ext)
	{
		const CStringW dir = Dir();
		if (dir.IsEmpty()) {
			return CStringW();
		}
		CStringW path;
		path.Format(L"%skhr_%016llx.%s", dir.GetString(), itemId, ext);
		return path;
	}

	unsigned long long TotalBytes()
	{
		unsigned long long total = 0;
		for (const auto& f : ListFiles()) {
			total += f.size;
		}
		return total;
	}

	void ClearAll()
	{
		int removed = 0, left = 0;
		for (const auto& f : ListFiles()) {
			if (DeleteFileW(f.path)) {
				removed++;
			} else {
				left++;
			}
		}
		if (removed || left) {
			KHLog::Format(L"cache: cleared %d file(s)%s", removed, left ? L" (some still in use, left for next time)" : L"");
		}
	}

	void Prune(const std::vector<unsigned long long>& keep, unsigned long long protectId, unsigned long long capBytes)
	{
		std::vector<CacheFile> rest;
		for (const auto& f : ListFiles()) {
			if (std::find(keep.begin(), keep.end(), f.id) == keep.end()) {
				if (DeleteFileW(f.path)) {
					KHLog::Format(L"cache: removed %s", f.path.GetString());
					continue;
				}
			}
			rest.push_back(f);
		}
		unsigned long long total = 0;
		for (const auto& f : rest) {
			total += f.size;
		}
		if (total <= capBytes) {
			return;
		}
		std::sort(rest.begin(), rest.end(), [](const CacheFile& a, const CacheFile& b) { return a.written < b.written; });
		for (const auto& f : rest) {
			if (total <= capBytes) {
				break;
			}
			if (f.id != protectId && DeleteFileW(f.path)) {
				total -= f.size;
				KHLog::Format(L"cache: over %llu MB - evicted %s", capBytes >> 20, f.path.GetString());
			}
		}
	}

	void WavHeader(unsigned char out[44], int channels, int sampleRate, unsigned long dataBytes)
	{
		memcpy(out, "RIFF", 4);
		Put32(out + 4, 36 + dataBytes);
		memcpy(out + 8, "WAVEfmt ", 8);
		Put32(out + 16, 16);
		Put16(out + 20, 1); // PCM
		Put16(out + 22, static_cast<unsigned>(channels));
		Put32(out + 24, static_cast<unsigned long>(sampleRate));
		Put32(out + 28, static_cast<unsigned long>(sampleRate) * static_cast<unsigned long>(channels) * 2);
		Put16(out + 32, static_cast<unsigned>(channels * 2));
		Put16(out + 34, 16);
		memcpy(out + 36, "data", 4);
		Put32(out + 40, dataBytes);
	}

	DecodeResult OggToWav(const CStringW& oggPath, const CStringW& wavPath, unsigned long long maxWavBytes,
		const std::function<bool()>& cancelled, DecodeInfo* info, double prefixSeconds)
	{
		DecodeInfo local;
		DecodeInfo& out = info ? *info : local;
		out = DecodeInfo();
		// a RIFF header can't describe more than 4 GB
		maxWavBytes = std::min<unsigned long long>(maxWavBytes, 0xFFFFFFFFull - 64);

		FILE* fp = nullptr;
		if (_wfopen_s(&fp, oggPath, L"rb") != 0 || !fp) {
			KHLog::Format(L"ogg: could not open %s", oggPath.GetString());
			return DecodeResult::Failed;
		}
		int err = 0;
		stb_vorbis* v = stb_vorbis_open_file(fp, 1, &err, nullptr); // owns fp from here, also on failure
		if (!v) {
			const bool notVorbis = err != VORBIS_unexpected_eof && err != VORBIS_outofmem && err != VORBIS_file_open_failure;
			KHLog::Format(L"ogg: %s (stb_vorbis error %d)", notVorbis ? L"not an Ogg Vorbis stream" : L"could not read the stream", err);
			return notVorbis ? DecodeResult::NotVorbis : DecodeResult::Failed;
		}
		const stb_vorbis_info vi = stb_vorbis_get_info(v);
		const int channels = vi.channels >= 2 ? 2 : 1;
		const int rate = static_cast<int>(vi.sample_rate);
		const unsigned long long total = stb_vorbis_stream_length_in_samples(v); // 0 if unknown
		const unsigned long long expected = total * static_cast<unsigned long long>(channels) * 2;
		out.channels = channels;
		out.sampleRate = rate;

		DecodeResult result = DecodeResult::Ok;
		if (rate <= 0 || rate > 384000) {
			result = DecodeResult::Failed;
		} else if (expected > maxWavBytes) {
			result = DecodeResult::TooBig;
		} else if (expected) {
			CStringW dir = wavPath;
			const int slash = dir.ReverseFind(L'\\');
			dir.Truncate(slash > 0 ? slash + 1 : 0);
			ULARGE_INTEGER freeBytes = {};
			if (!dir.IsEmpty() && GetDiskFreeSpaceExW(dir, &freeBytes, nullptr, nullptr)
					&& freeBytes.QuadPart < expected + (64ull << 20)) {
				KHLog::Format(L"ogg: not enough disk space for a %llu MB WAV", expected >> 20);
				result = DecodeResult::Failed;
			}
		}
		if (result != DecodeResult::Ok) {
			stb_vorbis_close(v);
			return result;
		}

		const CStringW part = wavPath + L".part";
		HANDLE h = CreateFileW(part, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			KHLog::Format(L"ogg: could not create %s (error %lu)", part.GetString(), GetLastError());
			stb_vorbis_close(v);
			return DecodeResult::Failed;
		}

		unsigned char header[44];
		WavHeader(header, channels, rate, 0);
		bool ok = WriteAll(h, header, sizeof(header));
		const size_t prefixFrames = static_cast<size_t>(std::max(0.0, prefixSeconds) * rate);
		out.monoPrefix.reserve(std::min<size_t>(prefixFrames, 48000u * 30u));
		// the talking check's second window: the middle of the track, when its length is known
		const unsigned long long midStart = total > 3ull * prefixFrames ? total / 2 : 0;
		std::vector<short> buf(4096u * static_cast<size_t>(channels));
		unsigned long long data = 0;
		while (ok) {
			if (cancelled && cancelled()) {
				result = DecodeResult::Cancelled;
				break;
			}
			const int n = stb_vorbis_get_samples_short_interleaved(v, channels, buf.data(), static_cast<int>(buf.size()));
			if (n <= 0) {
				break;
			}
			const DWORD bytes = static_cast<DWORD>(n) * static_cast<DWORD>(channels) * 2;
			if (data + bytes > maxWavBytes) {
				result = DecodeResult::TooBig;
				break;
			}
			if (!WriteAll(h, buf.data(), bytes)) {
				KHLog::Format(L"ogg: could not write %s (error %lu)", part.GetString(), GetLastError());
				ok = false;
				break;
			}
			for (int i = 0; i < n && out.monoPrefix.size() < prefixFrames; i++) {
				const float s = channels == 2
					? (buf[2 * static_cast<size_t>(i)] + buf[2 * static_cast<size_t>(i) + 1]) * (0.5f / 32768.0f)
					: buf[static_cast<size_t>(i)] / 32768.0f;
				out.monoPrefix.push_back(s);
			}
			if (midStart) {
				for (int i = 0; i < n && out.monoMiddle.size() < prefixFrames; i++) {
					if (out.frames + static_cast<unsigned long long>(i) >= midStart) {
						const float s = channels == 2
							? (buf[2 * static_cast<size_t>(i)] + buf[2 * static_cast<size_t>(i) + 1]) * (0.5f / 32768.0f)
							: buf[static_cast<size_t>(i)] / 32768.0f;
						out.monoMiddle.push_back(s);
					}
				}
			}
			data += bytes;
			out.frames += static_cast<unsigned long long>(n);
		}
		stb_vorbis_close(v);

		if (ok && result == DecodeResult::Ok) {
			// a stream that stops well short of its stated length is damaged
			const unsigned long long slack = std::max<unsigned long long>(4096, total / 50);
			if (out.frames == 0 || (total && out.frames + slack < total)) {
				KHLog::Format(L"ogg: decoded %llu of %llu samples - the stream is damaged", out.frames, total);
				ok = false;
			}
		}
		if (ok && result == DecodeResult::Ok) {
			WavHeader(header, channels, rate, static_cast<unsigned long>(data));
			LARGE_INTEGER zero = {};
			ok = SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) && WriteAll(h, header, sizeof(header)) && FlushFileBuffers(h);
		}
		CloseHandle(h);
		if (ok && result == DecodeResult::Ok && MoveFileExW(part, wavPath, MOVEFILE_REPLACE_EXISTING)) {
			KHLog::Format(L"ogg: decoded to %s (%d Hz, %d ch, %llu samples)", wavPath.GetString(), rate, channels, out.frames);
			return DecodeResult::Ok;
		}
		DeleteFileW(part);
		return result == DecodeResult::Ok ? DecodeResult::Failed : result;
	}
}
