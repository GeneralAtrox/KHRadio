// KH Radio - tests. Offline by default: the parser on small hand-written pages,
// share links (building, finding them in pasted text, refusing lookalikes),
// response classification, retry delays, layout geometry, history files, the
// talking-track filter (generated audio, title cues, album context, skipped
// queue rows) and the browser session's shutdown. Opt-in live checks against downloads.khinsider.com:
//   KHRadioTests --live     the saved sign-in (if any) reaches the site
//   KHRadioTests --login    opens the sign-in window and waits up to 10 minutes
// GPL-3.0-or-later (see COPYING.txt).
#include "stdafx.h"
#include "AppData.h"
#include "AudioCache.h"
#include "AudioClassify.h"
#include "FileUtil.h"
#include "History.h"
#include "KHParse.h"
#include "KHRadioLayout.h"
#include "KHWebSession.h"
#include "ShareLink.h"
#include "TextUtil.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <thread>
#include <shlobj.h>
#include <winhttp.h>

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond) \
	do { \
		if (cond) { g_passed++; } \
		else { g_failed++; fprintf(stderr, "FAIL %s(%d): %s\n", __FILE__, __LINE__, #cond); } \
	} while (0)

using namespace KHInsider;

static CStringW ExeDir()
{
	WCHAR path[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	PathRemoveFileSpecW(path);
	return CStringW(path) + L"\\";
}

static std::string Fixture(LPCWSTR name)
{
	std::vector<char> data;
	if (KHFile::ReadAll(ExeDir() + L"fixtures\\" + name, data) != KHFile::ReadResult::Ok) {
		fprintf(stderr, "FAIL: fixture %ls is missing\n", name);
		g_failed++;
		return std::string();
	}
	return std::string(data.data(), data.size() - 1);
}

static bool WriteText(const CStringW& path, const char* text)
{
	return KHFile::WriteAtomic(path, text, strlen(text));
}

static std::string ReadText(const CStringW& path)
{
	std::vector<char> data;
	return KHFile::ReadAll(path, data) == KHFile::ReadResult::Ok ? std::string(data.data(), data.size() - 1) : std::string("<missing>");
}

static int CountFiles(const CStringW& dir, LPCWSTR pattern)
{
	WIN32_FIND_DATAW fd = {};
	HANDLE h = FindFirstFileW(dir + pattern, &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return 0;
	}
	int n = 0;
	do { n++; } while (FindNextFileW(h, &fd));
	FindClose(h);
	return n;
}

static void RemoveTree(const CStringW& dir)
{
	WIN32_FIND_DATAW fd = {};
	HANDLE h = FindFirstFileW(dir + L"*", &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const CStringW name = fd.cFileName;
			if (name == L"." || name == L"..") {
				continue;
			}
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				RemoveTree(dir + name + L"\\");
			} else {
				DeleteFileW(dir + name);
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir);
}

static void TestAlbumPage()
{
	Album album;
	bool fallback = true;
	const CStringW url = L"https://downloads.khinsider.com/game-soundtracks/album/test-album";
	CHECK(ParseAlbumPage(Fixture(L"album.html"), url, album, &fallback));
	CHECK(!fallback);
	CHECK(album.url == url);
	CHECK(album.title == L"Test & Album: Sound's Edition");
	CHECK(album.platforms == L"Saturn, Arcade");
	CHECK(album.tracks.size() == 3);
	if (album.tracks.size() == 3) {
		CHECK(album.tracks[0].name == L"Opening");
		CHECK(album.tracks[0].number == 1);
		CHECK(album.tracks[0].durationSec == 83);
		CHECK(album.tracks[0].pageUrl == L"https://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3");
		CHECK(album.tracks[1].name == L"Stage 1 \"Forest\"");
		CHECK(album.tracks[1].durationSec == 3723);
		CHECK(album.tracks[2].name == L"Voice Collection");
		CHECK(album.tracks[2].number == 3);
		CHECK(album.tracks[2].durationSec == 12);
	}
	CHECK(album.coverUrls.size() == 4);
	if (album.coverUrls.size() == 4) {
		CHECK(album.coverUrls[0] == L"https://jetta.vgmtreasurechest.com/soundtracks/test-album/cover.jpg");
		CHECK(album.coverUrls[1] == L"https://jetta.vgmtreasurechest.com/soundtracks/test-album/back.png");
		CHECK(album.coverUrls[2] == L"https://jetta.vgmtreasurechest.com/soundtracks/test-album/thumbs/cover.jpg");
	}

	// no songlist table: only this album's own links count
	Album fb;
	CHECK(ParseAlbumPage(Fixture(L"album_nosonglist.html"), url, fb, &fallback));
	CHECK(fallback);
	CHECK(fb.tracks.size() == 2);
	if (fb.tracks.size() == 2) {
		CHECK(fb.tracks[0].name == L"First Song");
		CHECK(fb.tracks[1].name == L"Second Song");
	}
	CHECK(fb.title == L"Fallback Album");
	CHECK(fb.coverUrls.empty());

	// a page with nothing to play is a layout problem, not an album
	Album none;
	CHECK(!ParseAlbumPage(Fixture(L"album_empty.html"), url, none, nullptr));
	CHECK(none.tracks.empty());
	CHECK(none.title == L"Redesigned Page");
}

static void TestTrackPage()
{
	CHECK(ExtractTrackAudioUrl(Fixture(L"track_audio.html")) == L"https://jetta.vgmtreasurechest.com/soundtracks/test-album/abcd/01%20Opening.mp3");
	CHECK(ExtractTrackAudioUrl(Fixture(L"track_links.html")) == L"https://jetta.vgmtreasurechest.com/soundtracks/test-album/abcd/02.mp3");
	CHECK(ExtractTrackAudioUrl(Fixture(L"track_flac.html")) == L"https://jetta.vgmtreasurechest.com/soundtracks/test-album/abcd/03.flac");
	CHECK(ExtractTrackAudioUrl(Fixture(L"track_none.html")).IsEmpty());
	CHECK(ExtractTrackAudioUrl("").IsEmpty());

	// m4a / ogg when no mp3 is offered; mp3 still first when it is
	const CStringW base = L"https://jetta.vgmtreasurechest.com/soundtracks/test-album/abcd/";
	CHECK(ExtractTrackAudioUrl(Fixture(L"track_m4a.html")) == base + L"04.M4A?t=1");
	CHECK(ExtractTrackAudioUrl(Fixture(L"track_ogg.html")) == base + L"05.ogg");
	const std::string mixed = Fixture(L"track_mixed.html");
	CHECK(ExtractTrackAudioUrl(mixed) == base + L"06.mp3?x=2");
	CHECK(ExtractTrackAudioUrl(mixed, L"ogg") == base + L"06.ogg");
	CHECK(ExtractTrackAudioUrl(mixed, L"m4a") == base + L"06.m4a");
	CHECK(ExtractTrackAudioUrl(mixed, L"flac") == base + L"06.mp3?x=2"); // not offered: as usual
	CHECK(ExtractTrackAudioUrl(mixed, L"x\"y") == base + L"06.mp3?x=2");  // not an extension: ignored
	CHECK(ExtractTrackAudioUrl(Fixture(L"track_links.html"), L"flac") == base + L"02.flac");

	CHECK(AudioFormatOf(base + L"04.M4A?t=1") == AudioFormat::M4a);
	CHECK(AudioFormatOf(base + L"05.ogg") == AudioFormat::Ogg);
	CHECK(AudioFormatOf(base + L"05.OGA#x") == AudioFormat::Ogg);
	CHECK(AudioFormatOf(base + L"06.mp3?x=2") == AudioFormat::Mp3);
	CHECK(AudioFormatOf(base + L"03.flac") == AudioFormat::Flac);
	CHECK(AudioFormatOf(base + L"x.ogg.mp3") == AudioFormat::Mp3);
	CHECK(AudioFormatOf(base + L"x.mp3?f=a.ogg") == AudioFormat::Mp3);
	CHECK(AudioFormatOf(L"") == AudioFormat::Other);
}

// ---- a tiny Ogg Vorbis stream, built here (no encoder, no recorded audio) ----
// Mono, 8000 Hz, 256-sample blocks. Every audio packet sets one spectral line
// (bin 16 of 128) under a flat floor, which decodes to a steady ~516 Hz tone.

namespace {
	struct BitWriter {
		std::vector<unsigned char> bytes;
		int used = 0; // bits used in the last byte
		void Put(unsigned long long v, int n)
		{
			for (int i = 0; i < n; i++) {
				if (used == 0) {
					bytes.push_back(0);
				}
				if ((v >> i) & 1) {
					bytes.back() = static_cast<unsigned char>(bytes.back() | (1u << used));
				}
				used = (used + 1) & 7;
			}
		}
		void Header(int type)
		{
			Put(static_cast<unsigned>(type), 8);
			for (const char* p = "vorbis"; *p; p++) {
				Put(static_cast<unsigned char>(*p), 8);
			}
		}
	};

	unsigned long OggCrc(const std::vector<unsigned char>& page)
	{
		unsigned long crc = 0;
		for (unsigned char b : page) {
			crc ^= static_cast<unsigned long>(b) << 24;
			for (int k = 0; k < 8; k++) {
				crc = ((crc & 0x80000000ul) ? ((crc << 1) ^ 0x04C11DB7ul) : (crc << 1)) & 0xFFFFFFFFul;
			}
		}
		return crc;
	}

	void AddPage(std::vector<unsigned char>& out, unsigned char flags, unsigned long long granule, unsigned long seq,
		const std::vector<std::vector<unsigned char>>& packets, const char* magic = "OggS")
	{
		std::vector<unsigned char> lacing, body;
		for (const auto& pk : packets) {
			size_t n = pk.size();
			for (; n >= 255; n -= 255) {
				lacing.push_back(255);
			}
			lacing.push_back(static_cast<unsigned char>(n));
			body.insert(body.end(), pk.begin(), pk.end());
		}
		std::vector<unsigned char> page(magic, magic + 4);
		page.push_back(0);
		page.push_back(flags);
		for (int i = 0; i < 8; i++) {
			page.push_back(static_cast<unsigned char>(granule >> (8 * i)));
		}
		for (unsigned long v : { 0x4B485231ul, seq, 0ul }) { // serial, sequence, crc (filled below)
			for (int i = 0; i < 4; i++) {
				page.push_back(static_cast<unsigned char>(v >> (8 * i)));
			}
		}
		page.push_back(static_cast<unsigned char>(lacing.size()));
		page.insert(page.end(), lacing.begin(), lacing.end());
		page.insert(page.end(), body.begin(), body.end());
		const unsigned long crc = OggCrc(page);
		for (int i = 0; i < 4; i++) {
			page[22 + static_cast<size_t>(i)] = static_cast<unsigned char>(crc >> (8 * i));
		}
		out.insert(out.end(), page.begin(), page.end());
	}

	void PutCodebook(BitWriter& b, bool vq)
	{
		b.Put(0x564342, 24); // sync
		b.Put(1, 16);        // dimensions
		b.Put(2, 24);        // entries
		b.Put(0, 1);         // not ordered
		b.Put(0, 1);         // not sparse
		b.Put(0, 5);         // entry 0: length 1
		b.Put(0, 5);         // entry 1: length 1
		b.Put(vq ? 1 : 0, 4); // lookup type
		if (vq) {
			b.Put(0, 32);                            // minimum 0
			b.Put((767ull << 21) | (1ull << 20), 32); // delta 0.5
			b.Put(0, 4);                             // 1-bit values
			b.Put(0, 1);                             // not a sequence
			b.Put(0, 1);                             // multiplicand 0
			b.Put(1, 1);                             // multiplicand 1
		}
	}

	std::vector<unsigned char> MakeVorbis(int audioPackets)
	{
		BitWriter id;
		id.Header(1);
		id.Put(0, 32);    // version
		id.Put(1, 8);     // channels
		id.Put(8000, 32); // sample rate
		id.Put(0, 32);
		id.Put(0, 32);
		id.Put(0, 32);
		id.Put(8, 4);     // blocksize 0: 256
		id.Put(8, 4);     // blocksize 1: 256
		id.Put(1, 1);     // framing

		BitWriter comment;
		comment.Header(3);
		comment.Put(0, 32); // vendor length
		comment.Put(0, 32); // no comments
		comment.Put(1, 1);

		BitWriter setup;
		setup.Header(5);
		setup.Put(1, 8); // 2 codebooks
		PutCodebook(setup, false);
		PutCodebook(setup, true);
		setup.Put(0, 6);
		setup.Put(0, 16); // time domain placeholder
		setup.Put(0, 6);
		setup.Put(1, 16); // floor type 1
		setup.Put(0, 5);  // no partitions: just the two end points
		setup.Put(0, 2);  // multiplier 1
		setup.Put(7, 4);  // range bits: X1 = 128
		setup.Put(0, 6);
		setup.Put(1, 16); // residue type 1
		setup.Put(16, 24); // begin
		setup.Put(17, 24); // end
		setup.Put(0, 24);  // partition size 1
		setup.Put(0, 6);   // 1 classification
		setup.Put(0, 8);   // classbook 0
		setup.Put(1, 3);   // cascade: pass 0 only
		setup.Put(0, 1);
		setup.Put(1, 8);   // pass 0 uses book 1
		setup.Put(0, 6);
		setup.Put(0, 16);  // mapping type 0
		setup.Put(0, 1);   // one submap
		setup.Put(0, 1);   // no coupling
		setup.Put(0, 2);
		setup.Put(0, 8);   // time
		setup.Put(0, 8);   // floor 0
		setup.Put(0, 8);   // residue 0
		setup.Put(0, 6);   // one mode
		setup.Put(0, 1);   // short block
		setup.Put(0, 16);
		setup.Put(0, 16);
		setup.Put(0, 8);   // mapping 0
		setup.Put(1, 1);   // framing

		BitWriter audio;
		audio.Put(0, 1);   // audio packet (mode: 0 bits)
		audio.Put(1, 1);   // floor used
		audio.Put(255, 8); // Y0
		audio.Put(255, 8); // Y1
		audio.Put(0, 1);   // classification 0
		audio.Put(1, 1);   // VQ entry 1: 0.5 on bin 16

		std::vector<unsigned char> ogg;
		AddPage(ogg, 0x02, 0, 0, { id.bytes });
		AddPage(ogg, 0x00, 0, 1, { comment.bytes, setup.bytes });
		AddPage(ogg, 0x04, static_cast<unsigned long long>(audioPackets - 1) * 128, 2,
			std::vector<std::vector<unsigned char>>(static_cast<size_t>(audioPackets), audio.bytes));
		return ogg;
	}

	bool WriteBytes(const CStringW& path, const std::vector<unsigned char>& data)
	{
		FILE* f = nullptr;
		if (_wfopen_s(&f, path, L"wb") != 0 || !f) {
			return false;
		}
		const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
		fclose(f);
		return ok;
	}

	unsigned long Get32(const std::string& s, size_t at)
	{
		unsigned long v = 0;
		for (int i = 3; i >= 0; i--) {
			v = (v << 8) | static_cast<unsigned char>(s[at + static_cast<size_t>(i)]);
		}
		return v;
	}
}

static void TestAudioCache(const CStringW& dir)
{
	using namespace KHAudioCache;
	// what counts as a cache file: own names, directly in the folder
	const CStringW c = L"C:\\cache\\";
	CHECK(IsInside(c, L"C:\\cache\\khr_0123456789abcdef.wav"));
	CHECK(IsInside(c, L"c:\\CACHE\\KHR_0123456789ABCDEF.M4A"));
	CHECK(!IsInside(c, L"C:\\cache\\khr_0123456789abcdef.ogg"));
	CHECK(!IsInside(c, L"C:\\cache\\khr_0123456789abcdef.wav.part"));
	CHECK(!IsInside(c, L"C:\\cache\\khr_0123456789abcde.wav"));
	CHECK(!IsInside(c, L"C:\\cache\\sub\\khr_0123456789abcdef.wav"));
	CHECK(!IsInside(c, L"C:\\cache\\..\\khr_0123456789abcdef.wav"));
	CHECK(!IsInside(c, L"C:\\cachex\\khr_0123456789abcdef.wav"));
	CHECK(!IsInside(c, L"C:\\other\\khr_0123456789abcdef.wav"));
	CHECK(!IsInside(c, L"C:\\cache\\song.wav"));
	CHECK(!IsInside(L"", L"C:\\cache\\khr_0123456789abcdef.wav"));

	// the address route never takes a local file, cache or not
	CHECK(!IsAllowedMediaUrl(L"C:\\cache\\khr_0123456789abcdef.wav"));
	CHECK(!IsAllowedMediaUrl(L"file:///C:/cache/khr_0123456789abcdef.wav"));
	CHECK(!IsAllowedMediaUrl(L"\\\\server\\share\\khr_0123456789abcdef.wav"));

	// off until the app sets the folder; then only its own files
	SetDir(CStringW());
	CHECK(Dir().IsEmpty());
	CHECK(PathFor(1, L"wav").IsEmpty());
	CHECK(!IsCacheFile(L"C:\\cache\\khr_0123456789abcdef.wav"));
	const CStringW cache = dir + L"cache\\";
	CHECK(!SetDir(cache)); // missing
	CreateDirectoryW(cache, nullptr);
	CHECK(SetDir(cache));
	CHECK(Dir() == cache);
	const CStringW wav1 = PathFor(1, L"wav");
	CHECK(wav1 == cache + L"khr_0000000000000001.wav");
	CHECK(IsCacheFile(wav1));
	CHECK(!IsCacheFile(L"C:\\cache\\khr_0123456789abcdef.wav"));

	// prune: ids not kept go; over the cap the oldest go, never the protected one
	std::string kb(1024, 'x');
	CHECK(WriteText(PathFor(1, L"wav"), kb.c_str()));
	Sleep(20);
	CHECK(WriteText(PathFor(2, L"wav"), kb.c_str()));
	CHECK(WriteText(PathFor(3, L"m4a"), kb.c_str()));
	CHECK(WriteText(PathFor(3, L"7.ogg.part"), kb.c_str()));
	CHECK(WriteText(cache + L"keep-me.txt", "not ours"));
	CHECK(TotalBytes() == 4 * 1024);
	Prune({ 1, 2 }, 1, 1ull << 30);
	CHECK(CountFiles(cache, L"khr_*") == 2);
	Prune({ 1, 2 }, 1, 1500); // 2 KB held: the older one (1) is protected, so 2 goes
	CHECK(GetFileAttributesW(PathFor(1, L"wav")) != INVALID_FILE_ATTRIBUTES);
	CHECK(GetFileAttributesW(PathFor(2, L"wav")) == INVALID_FILE_ATTRIBUTES);
	ClearAll();
	CHECK(CountFiles(cache, L"khr_*") == 0);
	CHECK(GetFileAttributesW(cache + L"keep-me.txt") != INVALID_FILE_ATTRIBUTES); // never someone else's file
	SetDir(CStringW());

	unsigned char h[44];
	WavHeader(h, 2, 44100, 1000);
	const std::string hs(reinterpret_cast<const char*>(h), 44);
	CHECK(hs.compare(0, 4, "RIFF") == 0 && hs.compare(8, 8, "WAVEfmt ") == 0 && hs.compare(36, 4, "data") == 0);
	CHECK(Get32(hs, 4) == 1036 && Get32(hs, 24) == 44100 && Get32(hs, 28) == 176400 && Get32(hs, 40) == 1000);
	CHECK(h[22] == 2 && h[32] == 4 && h[34] == 16);
}

static void TestOggDecode(const CStringW& dir)
{
	using namespace KHAudioCache;
	const CStringW ogg = dir + L"tone.ogg";
	const CStringW wav = dir + L"tone.wav";
	const int packets = 64;
	CHECK(WriteBytes(ogg, MakeVorbis(packets)));

	DecodeInfo info;
	const DecodeResult r = OggToWav(ogg, wav, 1ull << 30, nullptr, &info);
	CHECK(r == DecodeResult::Ok);
	CHECK(info.channels == 1 && info.sampleRate == 8000);
	CHECK(info.frames + 300 >= static_cast<unsigned long long>(packets - 1) * 128 && info.frames <= static_cast<unsigned long long>(packets) * 128);
	CHECK(info.monoPrefix.size() == info.frames); // 1 s of audio < the 12 s prefix
	const std::string w = ReadText(wav);
	CHECK(w.size() == 44 + info.frames * 2);
	if (w.size() > 44 + 2048 && w.compare(0, 4, "RIFF") == 0) {
		CHECK(Get32(w, 24) == 8000 && static_cast<unsigned char>(w[22]) == 1);
		CHECK(Get32(w, 40) == w.size() - 44);
		// a steady tone near 516 Hz: ~0.129 zero crossings per sample
		const size_t n = (w.size() - 44) / 2;
		double sum = 0;
		int crossings = 0;
		short prev = 0;
		const size_t from = 512, to = n - 512;
		for (size_t i = from; i < to; i++) {
			const short s = static_cast<short>(static_cast<unsigned char>(w[44 + 2 * i]) | (static_cast<unsigned char>(w[45 + 2 * i]) << 8));
			sum += static_cast<double>(s) * s;
			if (i > from && ((s >= 0) != (prev >= 0))) {
				crossings++;
			}
			prev = s;
		}
		const double rms = sqrt(sum / static_cast<double>(to - from));
		const double zcr = crossings / static_cast<double>(to - from);
		CHECK(rms > 20.0);
		CHECK(zcr > 0.09 && zcr < 0.17);
		if (!(rms > 20.0 && zcr > 0.09 && zcr < 0.17)) {
			fprintf(stderr, "  tone: rms %.1f, zero crossings %.3f per sample\n", rms, zcr);
		}
	}
	CHECK(GetFileAttributesW(wav + L".part") == INVALID_FILE_ATTRIBUTES);

	// too long for the room given, cancelled: nothing left behind
	DeleteFileW(wav);
	CHECK(OggToWav(ogg, wav, 1000, nullptr, nullptr) == DecodeResult::TooBig);
	CHECK(OggToWav(ogg, wav, 1ull << 30, [] { return true; }, nullptr) == DecodeResult::Cancelled);
	CHECK(GetFileAttributesW(wav) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(wav + L".part") == INVALID_FILE_ATTRIBUTES);

	// not Vorbis: an Ogg Opus first page, and plain noise
	std::vector<unsigned char> opusHead = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0x38, 1, 0x80, 0xBB, 0, 0, 0, 0, 0 };
	std::vector<unsigned char> opus;
	AddPage(opus, 0x02, 0, 0, { opusHead });
	AddPage(opus, 0x04, 0, 1, { std::vector<unsigned char>(40, 7) });
	CHECK(WriteBytes(ogg, opus));
	CHECK(OggToWav(ogg, wav, 1ull << 30, nullptr, nullptr) == DecodeResult::NotVorbis);
	std::vector<unsigned char> noise(4096);
	unsigned seed = 12345;
	for (auto& b : noise) {
		seed = seed * 1103515245u + 12345u;
		b = static_cast<unsigned char>(seed >> 16);
	}
	CHECK(WriteBytes(ogg, noise));
	CHECK(OggToWav(ogg, wav, 1ull << 30, nullptr, nullptr) == DecodeResult::NotVorbis);
	CHECK(OggToWav(dir + L"missing.ogg", wav, 1ull << 30, nullptr, nullptr) == DecodeResult::Failed);
	CHECK(GetFileAttributesW(wav) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(wav + L".part") == INVALID_FILE_ATTRIBUTES);
	DeleteFileW(ogg);
}

static void TestHelpers()
{
	CHECK(BuildRandomAlbumForm({}, {}, {}) == "randomAdvanced=Show+Me+A+Random+Album");
	CHECK(BuildRandomAlbumForm({ 2, 1 }, { 1997 }, { 21, 0 })
		== "type%5B%5D=2&type%5B%5D=1&year%5B%5D=1997&category%5B%5D=21&category%5B%5D=0&randomAdvanced=Show+Me+A+Random+Album");

	CHECK(DecodeHtmlEntities(L"a &amp;quot; b &#x41;&#66; &lt;c&gt;") == L"a &quot; b AB <c>");

	CHECK(IsSpokenTitle(L"Drama CD Vol. 1"));
	CHECK(IsSpokenTitle(L"Voice Collection"));
	CHECK(IsSpokenTitle(L"ボイス集"));
	CHECK(!IsSpokenTitle(L"Green Hill Zone"));

	const unsigned char tag[] = { 'I', 'D', '3', 4, 0, 0x00, 0x00, 0x00, 0x02, 0x01 };
	CHECK(Id3v2TagSize(tag, sizeof(tag)) == 10 + 257);
	const unsigned char footer[] = { 'I', 'D', '3', 4, 0, 0x10, 0x00, 0x01, 0x00, 0x00 };
	CHECK(Id3v2TagSize(footer, sizeof(footer)) == 10 + 16384 + 10);
	const unsigned char bad[] = { 'I', 'D', '3', 4, 0, 0, 0x80, 0, 0, 0 };
	CHECK(Id3v2TagSize(bad, sizeof(bad)) == 0);
	const unsigned char mp3[] = { 0xFF, 0xFB, 0x90, 0x64, 0, 0, 0, 0, 0, 0 };
	CHECK(Id3v2TagSize(mp3, sizeof(mp3)) == 0);
	CHECK(Id3v2TagSize(tag, 5) == 0);

	CHECK(IsAllowedMediaUrl(L"https://jetta.vgmtreasurechest.com/a.mp3"));
	CHECK(IsAllowedMediaUrl(L"http://downloads.khinsider.com/a.jpg"));
	CHECK(IsAllowedMediaUrl(L"https://KHINSIDER.com:443/a"));
	CHECK(!IsAllowedMediaUrl(L"file:///C:/Windows/win.ini"));
	CHECK(!IsAllowedMediaUrl(L"https://evil.test/khinsider.com/a.mp3"));
	CHECK(!IsAllowedMediaUrl(L"https://khinsider.com.evil.test/a.mp3"));
	CHECK(!IsAllowedMediaUrl(L"https://khinsider.com@evil.test/a.mp3"));
	CHECK(!IsAllowedMediaUrl(L"https://notkhinsider.com/a.mp3"));
	CHECK(!IsAllowedMediaUrl(L"ftp://khinsider.com/a.mp3"));

	CHECK(AlbumExclusiveTo(L"Saturn", { L"Saturn", L"PS1" }));
	CHECK(AlbumExclusiveTo(L"Saturn, PS1", { L"saturn", L"PS1" }));
	CHECK(!AlbumExclusiveTo(L"Saturn, Arcade", { L"Saturn" }));
	CHECK(AlbumExclusiveTo(L"", { L"Saturn" }));
}

static void TestShareLinks()
{
	using KHShare::Link;
	using KHShare::LinkKind;
	const CStringW album = L"https://downloads.khinsider.com/game-soundtracks/album/test-album";
	const CStringW track = album + L"/01%2520Opening.mp3";
	Link link;

	// build -> parse round trip
	const CStringW text = KHShare::TrackShareText(L"Opening", L"Test Album", track);
	CHECK(text == L"Opening \x2014 Test Album\r\n" + track);
	CHECK(KHShare::FindLink(text, link) && link.kind == LinkKind::Track && link.albumUrl == album && link.trackKey == L"01 opening.mp3");
	const CStringW albumText = KHShare::AlbumShareText(L"Test Album", album);
	CHECK(albumText == L"Test Album\r\n" + album);
	CHECK(KHShare::FindLink(albumText, link) && link.kind == LinkKind::Album && link.albumUrl == album && link.trackKey.IsEmpty());
	CHECK(KHShare::TrackShareText(L"a", L"b", L"http://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3?x=1#t") == L"a \x2014 b\r\n" + track);
	CHECK(KHShare::TrackShareText(L"Two\r\nlines", L"", track) == L"Two  lines\r\n" + track);

	// a queue item restored without its page address: rebuilt from album + audio file
	const CStringW audio = L"https://vgmtreasurechest.com/soundtracks/test-album/abcdefgh/01%20Opening.mp3";
	CHECK(KHShare::TrackPageFor(track, album, audio) == track);
	CHECK(KHShare::TrackPageFor(L"", album, audio) == track);
	CHECK(KHShare::TrackPageFor(L"", album, audio + L"?t=1") == track);
	CHECK(KHShare::TrackPageFor(L"", album, L"").IsEmpty());
	CHECK(KHShare::TrackPageFor(L"", L"", audio).IsEmpty());
	CHECK(KHShare::TrackPageFor(L"", L"https://evil.example/game-soundtracks/album/test-album", audio).IsEmpty());
	CHECK(KHShare::TrackKeyOf(KHShare::TrackPageFor(L"", album, audio)) == L"01 opening.mp3");
	// only KHInsider pages are ever shared: never audio files on the CDN
	CHECK(KHShare::TrackShareText(L"a", L"b", L"https://jetta.vgmtreasurechest.com/soundtracks/test-album/abcd/01.mp3").IsEmpty());
	CHECK(KHShare::TrackShareText(L"a", L"b", album).IsEmpty());
	CHECK(KHShare::AlbumShareText(L"b", track).IsEmpty());
	CHECK(KHShare::AlbumShareText(L"b", L"").IsEmpty());

	// messy pasted text: the track is found in each
	const LPCWSTR messyTrack[] = {
		L"hey listen to this https://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3 so good",
		L"<https://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3>",
		L"Check it out: https://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3.",
		L"(https://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3)!",
		L"http://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3",
		L"www.downloads.khinsider.com/game-soundtracks/album/test-album/01%20Opening.mp3",
		L"downloads.khinsider.com/game-soundtracks/album/test-album/01%20Opening.mp3, enjoy",
		L"HTTPS://DOWNLOADS.KHINSIDER.COM/GAME-SOUNDTRACKS/ALBUM/test-album/01%2520Opening.mp3",
		L"https://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3?utm_source=chat#t=10",
		L"[Opening](https://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3)",
		L"[https://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3](https://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3)",
		L"<https://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3|Opening>",
		L"\"https://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3\"",
		L"**https://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3**",
		L"\x3053\x308Chttps://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3\x3002",
		L"skip https://evil.example/?u=https://downloads.khinsider.com/game-soundtracks/album/other/x.mp3 and "
			L"https://downloads.khinsider.com.evil.example/game-soundtracks/album/other then\r\n"
			L"https://downloads.khinsider.com/game-soundtracks/album/test-album/01%2520Opening.mp3",
	};
	for (const auto t : messyTrack) {
		const bool found = KHShare::FindLink(t, link);
		CHECK(found && link.kind == LinkKind::Track && link.albumUrl == album && link.trackKey == L"01 opening.mp3");
		if (!found || link.albumUrl != album) {
			fprintf(stderr, "  (text: %ls)\n", t);
		}
	}
	const LPCWSTR messyAlbum[] = {
		L"https://downloads.khinsider.com/game-soundtracks/album/test-album/",
		L"See https://downloads.khinsider.com/game-soundtracks/album/test-album?page=2.",
		L"https://downloads.khinsider.com/game-soundtracks/album/test%2Dalbum",
		L"album: <downloads.khinsider.com/game-soundtracks/album/test-album>",
	};
	for (const auto t : messyAlbum) {
		CHECK(KHShare::FindLink(t, link) && link.kind == LinkKind::Album && link.albumUrl == album);
	}

	// lookalikes, other schemes and other paths are refused before anything is fetched
	const LPCWSTR refused[] = {
		L"https://downloads.khinsider.com.evil.example/game-soundtracks/album/test-album",
		L"https://khinsider.com@evil.example/game-soundtracks/album/test-album",
		L"https://downloads.khinsider.com@evil.example/game-soundtracks/album/test-album",
		L"https://user:pw@downloads.khinsider.com/game-soundtracks/album/test-album",
		L"https://evil.example/downloads.khinsider.com/game-soundtracks/album/test-album",
		L"https://downloads.khinsider.com:8080/game-soundtracks/album/test-album",
		L"https://khinsider.com/game-soundtracks/album/test-album",
		L"https://notdownloads.khinsider.com/game-soundtracks/album/test-album",
		L"https://downloads.khinsider.co/game-soundtracks/album/test-album",
		L"ftp://downloads.khinsider.com/game-soundtracks/album/test-album",
		L"file://downloads.khinsider.com/game-soundtracks/album/test-album",
		L"javascript:alert(1)//downloads.khinsider.com/game-soundtracks/album/test-album",
		L"https://downloads.khinsider.com/game-soundtracks/browse/A",
		L"https://downloads.khinsider.com/game-soundtracks/album/",
		L"https://downloads.khinsider.com/game-soundtracks/album/a/b/c",
		L"https://downloads.khinsider.com/game-soundtracks/album/..%2Fsecret",
		L"https://downloads.khinsider.com/game-soundtracks/album//x.mp3",
		L"https://downloads.khinsider.com\\game-soundtracks/album/test-album",
		L"https://jetta.vgmtreasurechest.com/soundtracks/test-album/abcd/01.mp3",
		L"no link here",
		L"",
	};
	for (const auto t : refused) {
		const bool found = KHShare::FindLink(t, link);
		CHECK(!found && link.kind == LinkKind::None && link.albumUrl.IsEmpty());
		if (found) {
			fprintf(stderr, "  (accepted: %ls)\n", t);
		}
	}
	CHECK(!KHShare::ParseLink(L"https://downloads.khinsider.com.evil.example/game-soundtracks/album/x", link));
	CHECK(!KHShare::ParseLink(L"https://downloads.khinsider.com/game-soundtracks/album/test-album/..", link)); // in pasted text ".." reads as punctuation
	CHECK(!KHShare::ParseLink(L" https://downloads.khinsider.com/game-soundtracks/album/x", link));

	// album or track
	CHECK(KHShare::ParseLink(album, link) && link.kind == LinkKind::Album);
	CHECK(KHShare::ParseLink(album + L"/", link) && link.kind == LinkKind::Album);
	CHECK(KHShare::ParseLink(track, link) && link.kind == LinkKind::Track);
	CHECK(KHShare::ParseLink(track + L"/", link) && link.kind == LinkKind::Track);
	CHECK(KHShare::TrackKeyOf(album).IsEmpty());

	// finding the shared track on the album page, however its name was encoded
	std::vector<Track> tracks(3);
	tracks[0].pageUrl = album + L"/01%2520Opening.mp3";
	tracks[1].pageUrl = album + L"/02%2520Stage%25201%2520%2528Forest%2529.mp3";
	tracks[2].pageUrl = album + L"/03%2520%25E3%2583%259C.mp3";
	const auto find = [&](const CStringW& url) {
		Link l;
		return KHShare::ParseLink(url, l) ? KHShare::FindTrack(tracks, l.trackKey) : -2;
	};
	CHECK(find(album + L"/01%2520Opening.mp3") == 0);
	CHECK(find(album + L"/01%20OPENING.MP3") == 0);
	CHECK(find(album + L"/02%20Stage%201%20(Forest).mp3") == 1);
	CHECK(find(album + L"/02 Stage 1 (Forest).mp3") == 1);
	CHECK(find(album + L"/02%252520Stage%2525201%252520%252528Forest%252529.mp3") == 1); // encoded once more
	CHECK(find(album + L"/03%20%E3%83%9C.mp3") == 2);
	CHECK(find(album + L"/03%2520%25E3%2583%259C.mp3") == 2);
	CHECK(find(album + L"/03 \x30DC.mp3") == 2);
	CHECK(find(album + L"/09%2520Gone.mp3") == -1);
	CHECK(KHShare::FindTrack(tracks, L"") == -1);

	// and on a parsed album page: share each track, find it again
	Album parsed;
	CHECK(ParseAlbumPage(Fixture(L"album.html"), album, parsed, nullptr));
	for (size_t i = 0; i < parsed.tracks.size(); i++) {
		Link l;
		CHECK(KHShare::FindLink(KHShare::TrackShareText(parsed.tracks[i].name, parsed.title, parsed.tracks[i].pageUrl), l)
			&& l.albumUrl == album && KHShare::FindTrack(parsed.tracks, l.trackKey) == static_cast<int>(i));
	}
}

static void TestSession()
{
	using namespace KHWebSession;
	CHECK(IsSiteUrl(L"https://downloads.khinsider.com/random-album-advanced"));
	CHECK(!IsSiteUrl(L"http://downloads.khinsider.com/random-album-advanced"));
	CHECK(!IsSiteUrl(L"https://downloads.khinsider.com.evil.test/"));
	CHECK(!IsSiteUrl(L"https://downloads.khinsider.com@evil.test/"));
	CHECK(!IsSiteUrl(L"https://evil.test/downloads.khinsider.com/"));
	CHECK(ClassifyResponse(200, "<title>Please Log In</title>") == Status::LoginRequired);
	CHECK(ClassifyResponse(200, "To access the website's features, you need to be registered and logged in.") == Status::LoginRequired);
	CHECK(ClassifyResponse(200, "<form><input name=\"login\"><input name=\"password\"></form>") == Status::LoginRequired);
	CHECK(ClassifyResponse(401, "") == Status::LoginRequired);
	CHECK(ClassifyResponse(403, "") == Status::Blocked);
	CHECK(ClassifyResponse(429, "") == Status::Blocked);
	CHECK(ClassifyResponse(200, "<title>Just a moment...</title>") == Status::Blocked);
	CHECK(ClassifyResponse(503, "maintenance") == Status::NetworkError);
	CHECK(ClassifyResponse(0, "") == Status::NetworkError);
	CHECK(ClassifyResponse(200, "<a href='/forums/login'>Log In</a><table id='songlist'>") == Status::Ready);

	CHECK(RetryDelaySeconds(0) == 10);
	CHECK(RetryDelaySeconds(1) == 20);
	CHECK(RetryDelaySeconds(5) == 300);
	CHECK(RetryDelaySeconds(50) == 300);
	CHECK(RetryDelaySeconds(-3) == 10);
}

static void TestLayout()
{
	for (UINT dpi : { 96u, 144u, 192u }) {
		const SIZE content = { MulDiv(300, static_cast<int>(dpi), 96), MulDiv(720, static_cast<int>(dpi), 96) };
		const int bar = MulDiv(17, static_cast<int>(dpi), 96);
		const auto fit = KHRadioLayout::Measure(content, content, bar, bar);
		CHECK(!fit.horizontal && !fit.vertical);
		const auto shortPanel = KHRadioLayout::Measure({ content.cx + bar, content.cy / 2 }, content, bar, bar);
		CHECK(!shortPanel.horizontal && shortPanel.vertical);
		const auto narrowPanel = KHRadioLayout::Measure({ content.cx / 2, content.cy / 2 }, content, bar, bar);
		CHECK(narrowPanel.horizontal && narrowPanel.vertical);
		CHECK(narrowPanel.client.cx == content.cx / 2 - bar);
		CHECK(narrowPanel.client.cy == content.cy / 2 - bar);
		const RECT button = { 8, 280, 292, 312 };
		const auto placed = KHRadioLayout::Place(button, dpi, 96, content, { content.cx + 100, content.cy + 100 }, false);
		CHECK(placed.left == MulDiv(8, static_cast<int>(dpi), 96));
		CHECK(placed.right == MulDiv(292, static_cast<int>(dpi), 96) + 100);
		CHECK(placed.bottom - placed.top == MulDiv(312, static_cast<int>(dpi), 96) - MulDiv(280, static_cast<int>(dpi), 96));
		const auto history = KHRadioLayout::Place(button, dpi, 96, content, { content.cx + 100, content.cy + 100 }, true);
		CHECK(history.bottom == placed.bottom + 100);
	}
}

static void TestHistory(const CStringW& dir)
{
	using LR = CKHRadioHistory::LoadResult;

	// JSON round trip
	std::vector<CKHRadioHistory::AlbumEntry> albums(2);
	albums[0].url = L"https://downloads.khinsider.com/game-soundtracks/album/a";
	albums[0].title = L"Album \"A\" \x00e9";
	albums[0].tracks = { { L"One", 2 }, { L"Two", 1 } };
	albums[1].url = L"https://downloads.khinsider.com/game-soundtracks/album/b";
	albums[1].title = L"B";
	std::vector<CKHRadioHistory::AlbumEntry> back;
	CHECK(CKHRadioHistory::ParseJson(CKHRadioHistory::ToJson(albums).c_str(), back));
	CHECK(back.size() == 2 && back[0].title == albums[0].title && back[0].tracks.size() == 2
		&& back[0].tracks[0].plays == 2 && back[1].tracks.empty());
	CHECK(!CKHRadioHistory::ParseJson("{\"albums\": [ {\"url\": \"x\"", back));
	CHECK(!CKHRadioHistory::ParseJson("{\"albums\": 5}", back));
	CHECK(!CKHRadioHistory::ParseJson("", back));
	CHECK(CKHRadioHistory::ParseJson("{\"albums\": [ {\"title\": \"no url\"}, 7 ]}", back) && back.empty());

	const CStringW path = dir + L"history.json";
	{
		CKHRadioHistory h;
		CHECK(h.Load(path) == LR::Missing);
		CHECK(h.RecordTrack(albums[0].url, L"A", L"One") == -1);
		CHECK(h.RecordTrack(albums[1].url, L"B", L"Uno") == -1);
		CHECK(h.RecordTrack(albums[0].url, L"A", L"Two") == 1);
		CHECK(h.Albums().size() == 2 && h.Albums()[0].tracks.size() == 2);
		CHECK(KHFile::Exists(path) && KHFile::Exists(path + L".bak"));
	}
	{
		CKHRadioHistory h;
		CHECK(h.Load(path) == LR::Loaded);
		CHECK(h.Albums().size() == 2 && h.Albums()[0].title == L"A");
	}

	// a cut-short file is set aside and the backup is used
	const std::string good = ReadText(path);
	CHECK(WriteText(path, "{\"albums\": [ {\"url\": \"https://downloads"));
	{
		CKHRadioHistory h;
		CHECK(h.Load(path) == LR::RecoveredFromBackup);
		CHECK(!h.Albums().empty());
		CHECK(!h.IsReadOnly());
		CHECK(CountFiles(dir, L"history.json.corrupt-*") == 1);
	}

	// bad file and bad backup: start empty, both kept
	CHECK(WriteText(path, "not json"));
	CHECK(WriteText(path + L".bak", "also not json"));
	{
		CKHRadioHistory h;
		CHECK(h.Load(path) == LR::Corrupt);
		CHECK(h.Albums().empty());
		CHECK(!KHFile::Exists(path)); // renamed aside, nothing written over it
		CHECK(CountFiles(dir, L"history.json.corrupt-*") >= 1);
	}

	// an empty list never overwrites the file
	CHECK(WriteText(path, "{\"albums\": []}"));
	{
		CKHRadioHistory h;
		CHECK(h.Load(path) == LR::Loaded);
		CHECK(!h.Save());
		CHECK(ReadText(path) == "{\"albums\": []}");
	}

	// atomic write: the old content becomes the .bak, no .tmp is left behind
	CHECK(WriteText(path, good.c_str()));
	CHECK(KHFile::WriteAtomic(path, "{}", 2, true));
	CHECK(ReadText(path) == "{}");
	CHECK(ReadText(path + L".bak") == good);
	CHECK(!KHFile::Exists(path + L".tmp"));
}

static void PumpFor(DWORD ms)
{
	const ULONGLONG end = GetTickCount64() + ms;
	while (GetTickCount64() < end) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
	}
}

static void TestSessionShutdown(HWND owner, const CStringW& dir)
{
	using namespace KHWebSession;
	Initialize(owner, owner, std::wstring((dir + L"WebView2").GetString()));
	// Closing while a worker waits must release it, even before WebView2 starts.
	auto pending = std::async(std::launch::async, [] { return Request(L"https://downloads.khinsider.com/random-album-advanced"); });
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	Shutdown();
	const bool released = pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
	CHECK(released);
	if (released) {
		CHECK(pending.get().status != Status::Ready);
	}
	PumpFor(500); // let the browser process wind down before the profile is deleted
}

static int LiveCheck(HWND owner, const CStringW& dir, bool login)
{
	using namespace KHWebSession;
	Initialize(owner, owner, std::wstring((dir + L"WebView2").GetString()));
	if (login) {
		ShowLogin();
	}
	const ULONGLONG deadline = GetTickCount64() + (login ? 600000 : 60000);
	while (GetTickCount64() < deadline) {
		PumpFor(100);
		const Status state = GetStatus();
		if (state != Status::Starting && (!login || state == Status::Ready)) {
			break;
		}
	}
	printf("live: session status %d (Ready=1, LoginRequired=2)\n", static_cast<int>(GetStatus()));
	int failed = 0;
	if (GetStatus() == Status::Ready) {
		auto request = std::async(std::launch::async, [] {
			return Request(L"https://downloads.khinsider.com/random-album-advanced", "randomAdvanced=Show+Me+A+Random+Album");
		});
		while (request.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
			PumpFor(20);
		}
		const auto response = request.get();
		Album album;
		const bool parsed = response.status == Status::Ready
			&& ParseAlbumPage(response.html, CStringW(response.url.c_str()), album, nullptr);
		printf("live: random album %s (%u tracks)\n", parsed ? "parsed" : "NOT parsed", static_cast<unsigned>(album.tracks.size()));
		failed += parsed ? 0 : 1;
	} else if (GetStatus() != Status::LoginRequired) {
		failed++;
	}
	Shutdown();
	PumpFor(500);
	return failed;
}

// ---- evaluation mode (not run by default) ---------------------------------
// KHRadioTests --eval <list.tsv> <cache dir> <out.csv>
// The list holds one track per line, tab-separated: id, label (S talking /
// M music / Q unknown), half (tune/held), category, confidence, the 1.3 verdict,
// its logged flux, pause, zcrCoV and peak, the mp3 address, then free text.
// Each track's windows are fetched exactly as the app fetches them (ranged GETs,
// one at a time, paced, at most 150 MB in all) and kept in the cache directory,
// so a second run reads them from disk. Prints a short summary; the per-track
// measures go to out.csv.

namespace Eval
{
	constexpr unsigned long long kBudget = 150ull * 1024 * 1024;
	unsigned long long g_netBytes = 0;
	int g_netRequests = 0, g_cacheHits = 0, g_netFailures = 0;

	bool HttpRange(const CStringW& url, unsigned long long from, unsigned long long to, KHAudio::RangeReply& reply)
	{
		reply = KHAudio::RangeReply();
		WCHAR host[256] = {}, path[2048] = {};
		URL_COMPONENTSW uc = { sizeof(uc) };
		uc.lpszHostName = host;
		uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
		uc.lpszUrlPath = path;
		uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
		if (!WinHttpCrackUrl(url, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
			return false;
		}
		bool ok = false;
		DWORD status = 0;
		if (HINTERNET s = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)) {
			WinHttpSetTimeouts(s, 10000, 10000, 15000, 20000);
			if (HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0)) {
				if (HINTERNET r = WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
					CStringW headers;
					headers.Format(L"Accept: */*\r\nRange: bytes=%llu-%llu\r\n", from, to);
					if (WinHttpSendRequest(r, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
						DWORD len = sizeof(status);
						WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
						WCHAR cr[128] = {};
						len = sizeof(cr);
						if (status == 206 && WinHttpQueryHeaders(r, WINHTTP_QUERY_CUSTOM, L"Content-Range", cr, &len, WINHTTP_NO_HEADER_INDEX)) {
							const wchar_t* slash = wcschr(cr, L'/');
							reply.total = slash ? _wcstoui64(slash + 1, nullptr, 10) : 0;
							reply.partial = true;
						}
						const size_t want = static_cast<size_t>(to - from + 1);
						std::vector<char> buf(65536);
						while (status < 400 && reply.data.size() < want) {
							DWORD got = 0;
							if (!WinHttpReadData(r, buf.data(), static_cast<DWORD>(buf.size()), &got) || !got) {
								break;
							}
							const size_t take = std::min<size_t>(got, want - reply.data.size());
							reply.data.insert(reply.data.end(), buf.data(), buf.data() + take);
						}
						ok = status >= 200 && status < 300 && !reply.data.empty();
					}
					WinHttpCloseHandle(r);
				}
				WinHttpCloseHandle(c);
			}
			WinHttpCloseHandle(s);
		}
		return ok;
	}

	// Cached ranged GET: 16-byte header (total, partial) then the bytes.
	bool CachedRange(const CStringW& cacheDir, const CStringW& url, unsigned long long from, unsigned long long to, KHAudio::RangeReply& reply)
	{
		unsigned long long h = 1469598103934665603ull;
		for (int i = 0; i < url.GetLength(); i++) {
			h = (h ^ static_cast<unsigned long long>(url[i])) * 1099511628211ull;
		}
		CStringW file;
		file.Format(L"%s%016llx_%llu.bin", cacheDir.GetString(), h, from);
		std::vector<char> data;
		if (KHFile::ReadAll(file, data) == KHFile::ReadResult::Ok && data.size() > 17) {
			unsigned long long head[2] = {};
			memcpy(head, data.data(), sizeof(head));
			reply = KHAudio::RangeReply();
			reply.total = head[0];
			reply.partial = head[1] != 0;
			reply.data.assign(data.begin() + 16, data.end() - 1); // ReadAll adds a terminating zero
			g_cacheHits++;
			return true;
		}
		if (g_netBytes >= kBudget) {
			return false;
		}
		Sleep(400); // one request at a time, paced
		g_netRequests++;
		if (!HttpRange(url, from, to, reply)) {
			g_netFailures++;
			return false;
		}
		g_netBytes += reply.data.size();
		std::vector<char> out(16);
		const unsigned long long head[2] = { reply.total, reply.partial ? 1ull : 0ull };
		memcpy(out.data(), head, sizeof(head));
		out.insert(out.end(), reply.data.begin(), reply.data.end());
		KHFile::WriteAtomic(file, out.data(), out.size());
		return true;
	}

	std::vector<CStringW> SplitTabs(const CStringW& line)
	{
		std::vector<CStringW> parts;
		int start = 0;
		for (;;) {
			const int tab = line.Find(L'\t', start);
			if (tab < 0) {
				parts.push_back(line.Mid(start));
				return parts;
			}
			parts.push_back(line.Mid(start, tab - start));
			start = tab + 1;
		}
	}

	void PutFeatures(FILE* out, const KHAudio::Features& f)
	{
		fprintf(out, ",%d,%d,%.2f,%.4f,%.3f,%.3f,%.3f,%.4f,%.4f,%.3f,%.4f,%.4f,%.4f,%.4f", f.valid ? 1 : 0, f.quiet ? 1 : 0, f.seconds, f.peak,
			f.flux, f.pause, f.zcrCoV, f.tonal, f.mod4, f.envStd, f.beat, f.centroidCoV, f.gaps, f.score);
	}

	int Run(const CStringW& listPath, CStringW cacheDir, const CStringW& outPath)
	{
		if (cacheDir.Right(1) != L"\\") {
			cacheDir += L"\\";
		}
		SHCreateDirectoryExW(nullptr, cacheDir, nullptr);
		std::vector<char> raw;
		if (KHFile::ReadAll(listPath, raw) != KHFile::ReadResult::Ok) {
			fprintf(stderr, "eval: cannot read the list\n");
			return 2;
		}
		FILE* out = nullptr;
		if (_wfopen_s(&out, outPath, L"w") != 0 || !out) {
			fprintf(stderr, "eval: cannot write the output\n");
			return 2;
		}
		fputs("id,label,half,old_logged,old_flux_logged,bytes,windows,old_rule,score,verdict,strength,cue,album_spoken", out);
		for (int w = 1; w <= 2; w++) {
			fprintf(out, ",w%d_valid,w%d_quiet,w%d_sec,w%d_peak,w%d_flux,w%d_pause,w%d_zcrcov,w%d_tonal,w%d_mod4,w%d_envstd,w%d_beat,w%d_centcov,w%d_gaps,w%d_score",
				w, w, w, w, w, w, w, w, w, w, w, w, w, w);
		}
		fputs("\n", out);
		const CStringW text = UTF8ToWStr(raw.data());
		int pos = 0, tracks = 0;
		std::map<CStringW, std::array<int, 4>> tally; // label -> talking, silent, music, unjudged
		std::map<CStringW, std::array<int, 2>> oldTally; // label -> talking/silent by the 1.3 rule (recomputed), music
		for (CStringW line = text.Tokenize(L"\r\n", pos); pos >= 0; line = text.Tokenize(L"\r\n", pos)) {
			const auto f = SplitTabs(line);
			if (f.size() < 11 || !KHInsider::IsAllowedMediaUrl(f[10])) {
				continue;
			}
			const CStringW url = f[10];
			size_t bytes = 0;
			const auto d = KHAudio::ClassifyMp3([&](unsigned long long a, unsigned long long b, KHAudio::RangeReply& r) {
				return CachedRange(cacheDir, url, a, b, r);
			}, &bytes);
			tracks++;
			const bool oldSpoken = d.windows > 0 && (KHAudio::OldRuleSpoken(d.w[0]) || d.w[0].quiet);
			const int v = !d.judged ? 3 : d.verdict == KHInsider::AudioVerdict::Spoken ? 0 : d.verdict == KHInsider::AudioVerdict::Quiet ? 1 : 2;
			tally[f[1]][v]++;
			oldTally[f[1]][oldSpoken ? 0 : 1]++;
			const int cue = static_cast<int>(KHInsider::SpokenTitleCue(f.size() > 12 ? f[12] : CStringW()));
			const int albumSpoken = f.size() > 11 && KHInsider::IsSpokenTitle(f[11]) ? 1 : 0;
			fprintf(out, "%ls,%ls,%ls,%ls,%ls,%zu,%d,%d,%.4f,%d,%d,%d,%d", f[0].GetString(), f[1].GetString(), f[2].GetString(), f[5].GetString(), f[6].GetString(),
				bytes, d.windows, oldSpoken ? 1 : 0, d.score, v, static_cast<int>(d.strength), cue, albumSpoken);
			for (int w = 0; w < 2; w++) {
				PutFeatures(out, d.w[w]);
			}
			fputs("\n", out);
			fflush(out);
		}
		fclose(out);
		printf("eval: %d tracks; network %d requests, %.1f MB, %d failed; %d windows from the cache%s\n", tracks, g_netRequests,
			g_netBytes / 1048576.0, g_netFailures, g_cacheHits, g_netBytes >= kBudget ? " (BUDGET REACHED)" : "");
		for (const auto& [label, t] : tally) {
			printf("eval: label %ls: new talking %d, silent %d, music %d, unjudged %d | 1.3 rule (recomputed) dropped %d, kept %d\n",
				label.GetString(), t[0], t[1], t[2], t[3], oldTally[label][0], oldTally[label][1]);
		}
		return 0;
	}
}

// ---- the talking-track filter (1.4) ----------------------------------------

namespace Synth
{
	constexpr int kHz = 22050;
	constexpr double kTwoPi = 6.283185307179586;

	struct Rng {
		unsigned s;
		explicit Rng(unsigned seed) : s(seed * 2654435761u + 1u) {}
		double Next() { s = s * 1664525u + 1013904223u; return (s >> 8) / 8388608.0 - 1.0; } // -1..1
		double Uniform(double a, double b) { return a + (b - a) * (Next() * 0.5 + 0.5); }
	};

	std::vector<float> Silence(double sec) { return std::vector<float>(static_cast<size_t>(sec * kHz), 0.0f); }

	std::vector<float> Tone(double sec, double hzTone, double amp)
	{
		std::vector<float> x(static_cast<size_t>(sec * kHz));
		for (size_t i = 0; i < x.size(); i++) {
			x[i] = static_cast<float>(amp * std::sin(kTwoPi * hzTone * i / kHz));
		}
		return x;
	}

	// a held major chord with a kick on every beat and a hi-hat between
	std::vector<float> ChordWithBeat(double sec, unsigned seed)
	{
		Rng rng(seed);
		std::vector<float> x(static_cast<size_t>(sec * kHz));
		const double chord[] = { 261.6, 329.6, 392.0 };
		for (size_t i = 0; i < x.size(); i++) {
			const double t = static_cast<double>(i) / kHz;
			double v = 0.0;
			for (const double f : chord) {
				v += 0.08 * std::sin(kTwoPi * f * t);
			}
			const double inBeat = std::fmod(t, 0.5);
			v += 0.5 * std::exp(-inBeat * 30.0) * std::sin(kTwoPi * 60.0 * inBeat);
			const double inHat = std::fmod(t + 0.25, 0.5);
			v += 0.15 * std::exp(-inHat * 80.0) * rng.Next();
			x[i] = static_cast<float>(v);
		}
		return x;
	}

	// plucked notes that ring and fade: a music box or a piano line
	std::vector<float> MusicBox(double sec, unsigned seed)
	{
		Rng rng(seed);
		const double scale[] = { 523.3, 587.3, 659.3, 784.0, 880.0, 1046.5 };
		std::vector<float> x(static_cast<size_t>(sec * kHz));
		for (double start = 0.0; start < sec; start += 0.375) {
			const double f = scale[static_cast<int>(rng.Uniform(0, 5.99))];
			for (size_t i = static_cast<size_t>(start * kHz); i < x.size() && i < static_cast<size_t>((start + 1.5) * kHz); i++) {
				const double t = static_cast<double>(i) / kHz - start;
				x[i] += static_cast<float>(0.25 * std::exp(-t / 0.6) * (std::sin(kTwoPi * f * t) + 0.3 * std::sin(kTwoPi * 2 * f * t)));
			}
		}
		return x;
	}

	// a square-wave arpeggio: chiptune
	std::vector<float> SquareArpeggio(double sec)
	{
		const double notes[] = { 220.0, 277.2, 329.6, 440.0 };
		std::vector<float> x(static_cast<size_t>(sec * kHz));
		double phase = 0.0;
		for (size_t i = 0; i < x.size(); i++) {
			const int step = static_cast<int>(static_cast<double>(i) / kHz / 0.125);
			phase += notes[step % 4] / kHz;
			x[i] = static_cast<float>(std::fmod(phase, 1.0) < 0.5 ? 0.15 : -0.15);
		}
		return x;
	}

	// slow swelling held notes: ambient
	std::vector<float> AmbientPad(double sec)
	{
		const double notes[] = { 110.0, 164.8, 220.0, 277.2, 329.6 };
		std::vector<float> x(static_cast<size_t>(sec * kHz));
		for (size_t i = 0; i < x.size(); i++) {
			const double t = static_cast<double>(i) / kHz;
			double v = 0.0;
			for (int k = 0; k < 5; k++) {
				v += 0.05 * (0.6 + 0.4 * std::sin(kTwoPi * 0.15 * t + k)) * std::sin(kTwoPi * notes[k] * t);
			}
			x[i] = static_cast<float>(v);
		}
		return x;
	}

	std::vector<float> AmNoise(double sec, double rate, unsigned seed)
	{
		Rng rng(seed);
		std::vector<float> x(static_cast<size_t>(sec * kHz));
		for (size_t i = 0; i < x.size(); i++) {
			const double t = static_cast<double>(i) / kHz;
			x[i] = static_cast<float>(0.2 * (0.5 + 0.5 * std::sin(kTwoPi * rate * t)) * rng.Next());
		}
		return x;
	}

	std::vector<float> NoiseBursts(double sec, unsigned seed)
	{
		Rng rng(seed);
		std::vector<float> x(static_cast<size_t>(sec * kHz), 0.0f);
		for (double t = 0.0; t < sec;) {
			const double len = rng.Uniform(0.1, 0.4);
			for (size_t i = static_cast<size_t>(t * kHz); i < x.size() && i < static_cast<size_t>((t + len) * kHz); i++) {
				x[i] = static_cast<float>(0.25 * rng.Next());
			}
			t += len + rng.Uniform(0.2, 0.8);
		}
		return x;
	}

	// Speech-like: voiced syllables with a gliding pitch and moving harmonics,
	// short gaps between them, longer pauses between phrases, and a few hissy
	// consonants.
	std::vector<float> Talking(double sec, unsigned seed)
	{
		Rng rng(seed);
		std::vector<float> x(static_cast<size_t>(sec * kHz), 0.0f);
		double t = 0.2;
		int inPhrase = 0, phraseLen = 5;
		while (t < sec) {
			const double len = rng.Uniform(0.12, 0.30);
			const double f0 = rng.Uniform(110.0, 220.0), f1 = f0 * rng.Uniform(0.7, 1.3);
			double w[10];
			for (int k = 0; k < 10; k++) {
				w[k] = rng.Uniform(0.2, 1.0) / (k + 1);
			}
			double phase = 0.0;
			const size_t a = static_cast<size_t>(t * kHz), b = std::min(x.size(), static_cast<size_t>((t + len) * kHz));
			for (size_t i = a; i < b; i++) {
				const double u = static_cast<double>(i - a) / (b - a);
				phase += kTwoPi * (f0 + (f1 - f0) * u) / kHz;
				double v = 0.0;
				for (int k = 0; k < 10; k++) {
					v += w[k] * std::sin((k + 1) * phase);
				}
				x[i] = static_cast<float>(0.12 * std::sin(3.14159265 * u) * v);
			}
			t += len;
			if (rng.Uniform(0, 1) < 0.3) { // a consonant: a short hiss
				const size_t c = static_cast<size_t>(t * kHz), d = std::min(x.size(), c + static_cast<size_t>(0.07 * kHz));
				double prev = 0.0;
				for (size_t i = c; i < d; i++) {
					const double n = rng.Next();
					x[i] = static_cast<float>(0.08 * (n - prev));
					prev = n;
				}
				t += 0.07;
			}
			if (++inPhrase >= phraseLen) {
				inPhrase = 0;
				phraseLen = 3 + static_cast<int>(rng.Uniform(0, 5.99));
				t += rng.Uniform(0.3, 0.8);
			} else {
				t += rng.Uniform(0.03, 0.10);
			}
		}
		return x;
	}
}

// verbose: print every measure (KHRadioTests --synth)
static bool g_synthVerbose = false;

static KHAudio::Decision JudgeSynth(const std::vector<float>& first, const std::vector<float>& middle, const char* what)
{
	const auto d = KHAudio::ClassifyPcmWindows(first, middle, Synth::kHz);
	if (g_synthVerbose) {
		fprintf(stderr, "  synth %-22s %ls\n", what, KHAudio::Describe(d).GetString());
	}
	return d;
}

static void TestTalkingDetector()
{
	using namespace Synth;
	using KHInsider::AudioVerdict;
	const auto isMusic = [](const KHAudio::Decision& d) { return d.judged && d.verdict == AudioVerdict::Music; };

	CHECK(isMusic(JudgeSynth(Tone(12, 440, 0.3), Tone(12, 330, 0.3), "tone")));
	CHECK(isMusic(JudgeSynth(ChordWithBeat(12, 1), ChordWithBeat(12, 2), "chord with beat")));
	CHECK(isMusic(JudgeSynth(MusicBox(12, 3), MusicBox(12, 4), "music box / piano")));
	CHECK(isMusic(JudgeSynth(SquareArpeggio(12), SquareArpeggio(12), "chiptune arpeggio")));
	CHECK(isMusic(JudgeSynth(AmbientPad(12), AmbientPad(12), "ambient pad")));
	CHECK(isMusic(JudgeSynth(AmNoise(12, 4.0, 5), AmNoise(12, 4.0, 6), "4 Hz AM noise")));

	const auto bursts = JudgeSynth(NoiseBursts(12, 7), NoiseBursts(12, 8), "noise bursts + pauses");
	CHECK(bursts.judged && bursts.w[0].tonal < 0.3 && bursts.w[0].pause > 0.2);

	const auto talk = JudgeSynth(Talking(12, 9), Talking(12, 10), "talking");
	CHECK(talk.verdict == AudioVerdict::Spoken && talk.strength >= KHAudio::Strength::Moderate);

	// a spoken intro, then music in the middle: kept
	CHECK(isMusic(JudgeSynth(Talking(12, 11), ChordWithBeat(12, 12), "talking, then music")));
	// a medley of talking over a beat in one window is not "talking in every window"
	CHECK(isMusic(JudgeSynth(MusicBox(12, 13), Talking(12, 14), "music, then talking")));

	const auto silent = JudgeSynth(Silence(12), Silence(12), "silence");
	CHECK(silent.verdict == AudioVerdict::Quiet);
	// a quiet intro (the 1.3 filter dropped these as silent) and a loud middle
	std::vector<float> faint = Tone(12, 440, 0.005);
	const auto quietIntro = JudgeSynth(faint, ChordWithBeat(12, 15), "quiet intro, then loud");
	CHECK(quietIntro.verdict != AudioVerdict::Quiet && quietIntro.verdict != AudioVerdict::Spoken);
	// only one window that can be judged (a short track)
	const auto one = KHAudio::ClassifyPcmWindows(Tone(5, 440, 0.3), {}, kHz);
	CHECK(one.judged && one.windows == 1 && one.verdict == AudioVerdict::Music);
	// under a second: not judged, counts as music
	const auto tiny = KHAudio::ClassifyPcmWindows(Tone(0.5, 440, 0.3), {}, kHz);
	CHECK(!tiny.judged && tiny.verdict == AudioVerdict::Music);

	// Decide: every window must agree; the weaker window sets the strength
	KHAudio::Features a, b;
	a.valid = b.valid = true;
	a.score = 3.5; b.score = -0.5;
	KHAudio::Features both[2] = { a, b };
	CHECK(KHAudio::Decide(both, 2).verdict == AudioVerdict::Music);
	both[1].score = 2.7;
	CHECK(KHAudio::Decide(both, 2).strength == KHAudio::Strength::Strong);
	both[1].score = 3.2;
	CHECK(KHAudio::Decide(both, 2).strength == KHAudio::Strength::VeryStrong);
	both[1].score = 1.2;
	CHECK(KHAudio::Decide(both, 2).strength == KHAudio::Strength::Moderate);
	both[1].score = 0.4;
	CHECK(KHAudio::Decide(both, 2).strength == KHAudio::Strength::Weak);
	both[1].score = -0.4;
	CHECK(KHAudio::Decide(both, 2).strength == KHAudio::Strength::None);
	both[1].quiet = true; both[1].score = -1.0;
	CHECK(KHAudio::Decide(both, 2).verdict == AudioVerdict::Music); // a silent window is not talking

	// a long track sampled along its length: talking at a quarter of the points is enough
	{
		KHAudio::Features s[KHAudio::kSpreadWindows];
		for (auto& f : s) { f.valid = true; f.score = -4.0; }
		KHAudio::Features musicPair[2] = { a, b }; // 3.5 and -0.5: music by the two-window rule
		auto d = KHAudio::Decide(musicPair, 2);
		KHAudio::ApplySpread(d, s, KHAudio::kSpreadWindows);
		CHECK(d.spreadJudged == 16 && d.spreadTalking == 0 && !d.spreadTalk && d.verdict == AudioVerdict::Music);
		s[1].score = s[5].score = s[9].score = 2.0;
		KHAudio::ApplySpread(d, s, KHAudio::kSpreadWindows);
		CHECK(d.spreadTalking == 3 && !d.spreadTalk && d.verdict == AudioVerdict::Music); // 3 of 16: kept
		s[13].score = 1.0;
		KHAudio::ApplySpread(d, s, KHAudio::kSpreadWindows);
		CHECK(d.spreadTalking == 4 && d.spreadTalk && d.verdict == AudioVerdict::Spoken);
		KHAudio::AlbumTally musicalAlbum;
		musicalAlbum.judged = 8;
		CHECK(KHAudio::ApplyAlbumContext(d, KHInsider::TitleCue::None, musicalAlbum).skip == SkipReason::Talking);
		// a weak score or a silent window is not talking; too few judged windows decide nothing
		s[13].score = 0.9;
		auto weakOne = KHAudio::Decide(musicPair, 2);
		KHAudio::ApplySpread(weakOne, s, KHAudio::kSpreadWindows);
		CHECK(!weakOne.spreadTalk);
		s[13].score = 3.0; s[13].quiet = true;
		KHAudio::ApplySpread(weakOne, s, KHAudio::kSpreadWindows);
		CHECK(!weakOne.spreadTalk);
		for (int i = 0; i < 7; i++) { s[i].score = 2.0; }
		auto few = KHAudio::Decide(musicPair, 2);
		KHAudio::ApplySpread(few, s, 7);
		CHECK(few.spreadJudged == 7 && !few.spreadTalk && few.verdict == AudioVerdict::Music);
		// placement: none under 8 MB, otherwise the middle of each sixteenth
		CHECK(KHAudio::SpreadWindowStart(8ull * 1024 * 1024 - 1, 0, 0) == 0);
		CHECK(KHAudio::SpreadWindowStart(16ull * 1024 * 1024 + 1000, 1000, 0) == 1000 + 524288 - 65536);
		CHECK(KHAudio::SpreadWindowStart(16ull * 1024 * 1024 + 1000, 1000, 15) == 1000 + 15ull * 1048576 + 524288 - 65536);
		CHECK(KHAudio::SpreadWindowStart(16ull * 1024 * 1024, 0, 16) == 0);
	}

	// the middle window: placed from the file size, none when the file is short
	CHECK(KHAudio::MiddleWindowStart(0, 0, KHAudio::kWindowBytes) == 0);
	CHECK(KHAudio::MiddleWindowStart(400000, 0, KHAudio::kWindowBytes) == 0);
	CHECK(KHAudio::MiddleWindowStart(4000000, 1000, KHAudio::kWindowBytes) == 1000 + (4000000 - 1000) / 2 - KHAudio::kWindowBytes / 2);
	std::vector<float> mono;
	int hz = 0;
	const unsigned char junk[64] = { 1, 2, 3 };
	CHECK(!KHAudio::DecodeMp3(junk, sizeof(junk), 0, 30.0, mono, hz));
}

static void TestAlbumContext()
{
	using KHInsider::AudioVerdict;
	using KHInsider::TitleCue;
	const auto talking = [](KHAudio::Strength s) {
		KHAudio::Decision d;
		d.judged = true;
		d.windows = 2;
		d.verdict = s == KHAudio::Strength::None ? AudioVerdict::Music : AudioVerdict::Spoken;
		d.strength = s;
		return d;
	};
	KHAudio::AlbumTally fresh, musical, drama;
	for (int i = 0; i < 6; i++) {
		musical.Add(talking(KHAudio::Strength::None));
	}
	for (int i = 0; i < 3; i++) {
		drama.Add(talking(KHAudio::Strength::Moderate));
	}
	drama.Add(talking(KHAudio::Strength::None));
	CHECK(musical.Musical() && !musical.MostlyTalking());
	CHECK(drama.MostlyTalking() && !drama.Musical());
	CHECK(!fresh.Musical() && !fresh.MostlyTalking());

	const auto skip = [](const KHAudio::Decision& d, TitleCue cue, const KHAudio::AlbumTally& t) {
		return KHAudio::ApplyAlbumContext(d, cue, t).skip;
	};
	const auto weak = talking(KHAudio::Strength::Weak);
	CHECK(skip(weak, TitleCue::None, fresh) == SkipReason::None);
	CHECK(skip(weak, TitleCue::None, drama) == SkipReason::None);
	CHECK(skip(weak, TitleCue::Weak, musical) == SkipReason::Talking); // "#scene12" in a soundtrack
	const auto mod = talking(KHAudio::Strength::Moderate), strong = talking(KHAudio::Strength::Strong),
		very = talking(KHAudio::Strength::VeryStrong), music = talking(KHAudio::Strength::None);
	// a fresh album: strong talking on its own
	CHECK(skip(mod, TitleCue::None, fresh) == SkipReason::None);
	CHECK(skip(strong, TitleCue::None, fresh) == SkipReason::Talking);
	// a title cue lowers the bar to moderate, but never drops music
	CHECK(skip(mod, TitleCue::Weak, fresh) == SkipReason::Talking);
	CHECK(skip(music, TitleCue::Weak, fresh) == SkipReason::None);
	// one or two odd tracks in a musical album are kept unless very strong
	CHECK(skip(strong, TitleCue::None, musical) == SkipReason::None);
	CHECK(skip(very, TitleCue::None, musical) == SkipReason::Talking);
	CHECK(skip(mod, TitleCue::Weak, musical) == SkipReason::Talking); // "Scene 4" in a soundtrack
	// an album that is mostly talking: moderate is enough
	CHECK(skip(mod, TitleCue::None, drama) == SkipReason::Talking);
	CHECK(skip(music, TitleCue::None, drama) == SkipReason::None);
	// silent and not judged
	KHAudio::Decision quiet;
	quiet.judged = true;
	quiet.verdict = AudioVerdict::Quiet;
	CHECK(skip(quiet, TitleCue::None, musical) == SkipReason::Silent);
	CHECK(skip(KHAudio::Decision(), TitleCue::Strong, drama) == SkipReason::None);
	// the tally ignores silent and unjudged tracks
	KHAudio::AlbumTally t;
	t.Add(quiet);
	t.Add(KHAudio::Decision());
	CHECK(t.judged == 0);
}

static void TestTitleCues()
{
	using KHInsider::TitleCue;
	CStringW m;
	// strong: enough on their own (invented titles)
	CHECK(SpokenTitleCue(L"Bonus Drama CD Part 1") == TitleCue::Strong);
	CHECK(SpokenTitleCue(L"Mini Drama \"The Lost Key\"") == TitleCue::Strong);
	CHECK(SpokenTitleCue(L"Cast Comment (Hero & Rival)", &m) == TitleCue::Strong && m == L"cast comment");
	CHECK(SpokenTitleCue(L"ミニドラマ「星の庭」") == TitleCue::Strong);
	CHECK(SpokenTitleCue(L"ボイスドラマ 第1話") == TitleCue::Strong);
	CHECK(SpokenTitleCue(L"Voice Collection 2") == TitleCue::Strong);
	// weak: the audio must agree
	CHECK(SpokenTitleCue(L"Scene 12 - The Bridge") == TitleCue::Weak);
	CHECK(SpokenTitleCue(L"#scene4b") == TitleCue::Weak);
	CHECK(SpokenTitleCue(L"Drama Part 3") == TitleCue::Strong);
	CHECK(SpokenTitleCue(L"Afterword Drama") == TitleCue::Weak);
	CHECK(SpokenTitleCue(L"Message from the Cast") == TitleCue::Weak);
	CHECK(SpokenTitleCue(L"Free Talk") == TitleCue::Strong);
	CHECK(SpokenTitleCue(L"Midnight Talk") == TitleCue::Weak);
	CHECK(SpokenTitleCue(L"Opening Cutscene") == TitleCue::Weak);
	CHECK(SpokenTitleCue(L"Lumi Voice") == TitleCue::Weak);
	CHECK(SpokenTitleCue(L"Skit 05") == TitleCue::Weak);
	CHECK(SpokenTitleCue(L"ドラマ「夏の終わり」") == TitleCue::Weak);
	CHECK(SpokenTitleCue(L"第3話 迷子の猫") == TitleCue::Weak);
	CHECK(SpokenTitleCue(L"第十二話") == TitleCue::Weak);
	CHECK(SpokenTitleCue(L"キャラクターメッセージ") == TitleCue::Weak);
	CHECK(SpokenTitleCue(L"セリフ集 Vol.2") == TitleCue::Strong);
	CHECK(SpokenTitleCue(L"ナレーション") == TitleCue::Weak);
	// music titles that must not match
	CHECK(SpokenTitleCue(L"Dramatic Escape") == TitleCue::None);
	CHECK(SpokenTitleCue(L"ドラマチック・ナイト") == TitleCue::None);
	CHECK(SpokenTitleCue(L"ドラマティック") == TitleCue::None);
	CHECK(SpokenTitleCue(L"ドラマーの夢") == TitleCue::None);
	CHECK(SpokenTitleCue(L"Bike Ride (Skitchin' Mix)") == TitleCue::None);
	CHECK(SpokenTitleCue(L"Hall of Voices") == TitleCue::None);
	CHECK(SpokenTitleCue(L"A voice calls me home") == TitleCue::None);
	CHECK(SpokenTitleCue(L"Voice of the Deep") == TitleCue::None);
	CHECK(SpokenTitleCue(L"Inner Voice") == TitleCue::None);
	CHECK(SpokenTitleCue(L"Talkative Town") == TitleCue::None);
	CHECK(SpokenTitleCue(L"Messages in the Sand") == TitleCue::None);
	CHECK(SpokenTitleCue(L"Behind the Scenes") == TitleCue::None);
	CHECK(SpokenTitleCue(L"Final Scene") == TitleCue::None);
	CHECK(SpokenTitleCue(L"Seen and Unseen") == TitleCue::None);
	CHECK(SpokenTitleCue(L"第2章の扉") == TitleCue::Weak); // chapters: weak only, the audio decides
	CHECK(SpokenTitleCue(L"Green Hill Zone") == TitleCue::None);
	// album titles: a drama / voice album is rerolled
	CHECK(IsSpokenTitle(L"Starfall Drama CD Vol. 1"));
	CHECK(IsSpokenTitle(L"Starfall Radio DJCD"));
	CHECK(IsSpokenTitle(L"Moonlit Garden Mini Story CD"));
	CHECK(IsSpokenTitle(L"ドラマCD 星の庭"));
	CHECK(!IsSpokenTitle(L"Dramatic Orchestra Arrange"));
	CHECK(!IsSpokenTitle(L"Starfall Original Soundtrack"));
	CHECK(!IsSpokenTitle(L"Voices of the Lighthouse"));
	// short tracks: unknown length is never short, 0 turns it off
	CHECK(IsShortTrack(5, 10));
	CHECK(!IsShortTrack(10, 10));
	CHECK(!IsShortTrack(0, 10));
	CHECK(!IsShortTrack(5, 0));
}

static void TestSkippedQueue(const CStringW& dir)
{
	std::vector<QueueItem> q(6);
	for (size_t i = 0; i < q.size(); i++) {
		q[i].id = 100 + i;
		q[i].pageUrl.Format(L"https://downloads.khinsider.com/game-soundtracks/album/test-album/%02d.mp3", static_cast<int>(i));
		q[i].label.Format(L"%02d. Track", static_cast<int>(i));
	}
	q[1].skip = SkipReason::Talking;
	q[2].skip = SkipReason::Short;
	q[5].skip = SkipReason::Title;
	CHECK(StepPlayable(q, 0, 1) == 3);
	CHECK(StepPlayable(q, 3, -1) == 0);
	CHECK(StepPlayable(q, 4, 1) == -1);
	CHECK(StepPlayable(q, 0, -1) == -1);
	CHECK(PlayableAtOrAfter(q, 1) == 3);
	CHECK(PlayableAtOrAfter(q, 0) == 0);
	CHECK(PlayableAtOrAfter(q, 5) == -1);
	CHECK(AheadLimit(q, 0, 1) == 3);          // the skipped rows don't use up the window
	CHECK(AheadLimit(q, 0, 2) == 4);
	CHECK(AheadLimit(q, 0, 3) == 5);          // nothing playable left: to the end
	CHECK(CountPlayableAfter(q, 0) == 2);
	CHECK(CountPlayableAfter(q, -1) == 3);

	// queue.json keeps the skipped rows and "Play anyway"
	q[4].keep = true;
	const CStringW path = dir + L"queue-test.json";
	CHECK(KHRadioData::SaveQueue(path, q, 3, 12.5));
	const auto loaded = KHRadioData::LoadQueue(path);
	CHECK(loaded.items.size() == q.size());
	CHECK(loaded.index == 3);
	if (loaded.items.size() == q.size()) {
		CHECK(loaded.items[1].skip == SkipReason::Talking);
		CHECK(loaded.items[2].skip == SkipReason::Short);
		CHECK(loaded.items[5].skip == SkipReason::Title);
		CHECK(loaded.items[0].skip == SkipReason::None && !loaded.items[0].keep);
		CHECK(loaded.items[4].keep);
		CHECK(!loaded.items[1].judged);
	}
	CHECK(SkipReasonFromKey(L"talking") == SkipReason::Talking);
	CHECK(SkipReasonFromKey(L"nonsense") == SkipReason::None);

	// the "Play anyway" list: page addresses, case-blind, KHInsider only
	const CStringW keepPath = dir + L"keep-test.json";
	KHRadioData::KeepList keep;
	keep.insert(KHRadioData::KeepKey(L"https://downloads.khinsider.com/game-soundtracks/album/Test-Album/03.mp3"));
	CHECK(KHRadioData::SaveKeepList(keepPath, keep));
	const auto back = KHRadioData::LoadKeepList(keepPath);
	CHECK(back.size() == 1);
	CHECK(back.count(KHRadioData::KeepKey(L"https://downloads.khinsider.com/game-soundtracks/album/test-album/03.mp3")) == 1);
	CHECK(WriteText(keepPath, "{\"tracks\":[\"https://example.com/x\",\"https://downloads.khinsider.com/a\",7]}"));
	CHECK(KHRadioData::LoadKeepList(keepPath).size() == 1);
	CHECK(KHRadioData::LoadKeepList(dir + L"missing.json").empty());
}

// ---- scan mode (not run by default) ---------------------------------------
// KHRadioTests --scan <file.mp3> [window seconds]
// Decodes a whole mp3 from disk and prints the talking score of every window
// along its length, the share of windows at each strength, and the verdict the
// app's own route gives when its ranged reads are served from the file.
namespace Scan
{
	int Run(const wchar_t* path, double windowSec)
	{
		std::vector<char> data;
		if (KHFile::ReadAll(path, data, 256u * 1024 * 1024) != KHFile::ReadResult::Ok || data.size() < 4096) {
			fputs("FAIL: cannot read the file\n", stderr);
			return 2;
		}
		const auto* u = reinterpret_cast<const unsigned char*>(data.data());
		const size_t tag = KHInsider::Id3v2TagSize(u, data.size());
		std::vector<float> mono;
		int hz = 0;
		if (tag >= data.size() || !KHAudio::DecodeMp3(u + tag, data.size() - tag, 0, 7200.0, mono, hz)) {
			fputs("FAIL: cannot decode the file\n", stderr);
			return 2;
		}
		const size_t win = static_cast<size_t>(windowSec * hz);
		int n = 0, weak = 0, moderate = 0, strong = 0, quiet = 0;
		for (size_t at = 0; at + win <= mono.size(); at += win) {
			const KHAudio::Features f = KHAudio::Analyse(mono.data() + at, win, hz);
			n++;
			quiet += f.quiet ? 1 : 0;
			weak += !f.quiet && f.score >= KHAudio::kWeakScore ? 1 : 0;
			moderate += !f.quiet && f.score >= KHAudio::kModerateScore ? 1 : 0;
			strong += !f.quiet && f.score >= KHAudio::kStrongScore ? 1 : 0;
			printf("%5.0f s=%+.2f tonal=%.2f beat=%.2f pause=%.2f gaps=%.2f env=%.1f mod4=%.2f flux=%.1f zcr=%.2f cent=%.2f%s\n",
				static_cast<double>(at) / hz, f.score, f.tonal, f.beat, f.pause, f.gaps, f.envStd, f.mod4, f.flux, f.zcrCoV, f.centroidCoV,
				f.quiet ? " QUIET" : "");
		}
		printf("windows=%d (%.0f s each, %.0f s in all) weak+=%d moderate+=%d strong+=%d quiet=%d\n",
			n, windowSec, static_cast<double>(mono.size()) / hz, weak, moderate, strong, quiet);
		// the app's own route, with ranges served from the file
		const size_t size = data.size() - 1; // ReadAll adds a terminating zero
		size_t fetched = 0;
		const auto d = KHAudio::ClassifyMp3([&](unsigned long long from, unsigned long long to, KHAudio::RangeReply& r) {
			r = KHAudio::RangeReply();
			if (from >= size) {
				return false;
			}
			const size_t end = static_cast<size_t>(std::min<unsigned long long>(to + 1, size));
			r.data.assign(data.begin() + static_cast<std::ptrdiff_t>(from), data.begin() + static_cast<std::ptrdiff_t>(end));
			r.total = size;
			r.partial = true;
			return true;
		}, &fetched);
		const auto call = KHAudio::ApplyAlbumContext(d, KHInsider::TitleCue::None, KHAudio::AlbumTally());
		printf("app route (%zu KB fetched): %ls -> %ls (%ls)\n", fetched / 1024, KHAudio::Describe(d).GetString(),
			call.skip == SkipReason::None ? L"keep" : L"skip", call.rule);
		return 0;
	}
}

int wmain(int argc, wchar_t** argv)
{
	if (argc >= 3 && wcscmp(argv[1], L"--scan") == 0) {
		return Scan::Run(argv[2], argc >= 4 ? std::max(2.0, _wtof(argv[3])) : 8.0);
	}
	if (argc >= 5 && wcscmp(argv[1], L"--eval") == 0) {
		return Eval::Run(argv[2], argv[3], argv[4]);
	}
	if (!AfxWinInit(GetModuleHandleW(nullptr), nullptr, GetCommandLineW(), 0)) {
		fputs("FAIL: MFC did not start\n", stderr);
		return 2;
	}
	if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
		fputs("FAIL: COM did not start\n", stderr);
		return 2;
	}

	WCHAR temp[MAX_PATH] = {};
	GetTempPathW(MAX_PATH, temp);
	CStringW dir;
	dir.Format(L"%skhradio-tests-%lu\\", temp, GetCurrentProcessId());
	RemoveTree(dir);
	CreateDirectoryW(dir, nullptr);

	TestAlbumPage();
	TestTrackPage();
	TestHelpers();
	TestShareLinks();
	TestSession();
	TestLayout();
	TestHistory(dir);
	TestAudioCache(dir);
	TestOggDecode(dir);
	g_synthVerbose = argc > 1 && wcscmp(argv[1], L"--synth") == 0;
	TestTalkingDetector();
	TestAlbumContext();
	TestTitleCues();
	TestSkippedQueue(dir);


	HWND owner = CreateWindowExW(0, L"STATIC", L"KH Radio tests", WS_OVERLAPPEDWINDOW,
		0, 0, 600, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	CHECK(owner != nullptr);
	if (owner) {
		TestSessionShutdown(owner, dir);
	}

	int liveFailed = 0;
	const bool live = argc > 1 && (wcscmp(argv[1], L"--live") == 0 || wcscmp(argv[1], L"--login") == 0);
	if (live && owner) {
		liveFailed = LiveCheck(owner, dir, wcscmp(argv[1], L"--login") == 0);
	}
	if (owner) {
		DestroyWindow(owner);
	}

	RemoveTree(dir);
	CoUninitialize();
	printf("tests: %d passed, %d failed%s\n", g_passed, g_failed,
		live ? (liveFailed ? " (live check FAILED)" : " (live check passed)") : "");
	return (g_failed || liveFailed) ? 1 : 0;
}
