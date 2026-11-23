// KH Radio - tests. Offline by default: the parser on small hand-written pages,
// response classification, retry delays, layout geometry, history files and the
// browser session's shutdown. Opt-in live checks against downloads.khinsider.com:
//   KHRadioTests --live     the saved sign-in (if any) reaches the site
//   KHRadioTests --login    opens the sign-in window and waits up to 10 minutes
// GPL-3.0-or-later (see COPYING.txt).
#include "stdafx.h"
#include "AudioCache.h"
#include "FileUtil.h"
#include "History.h"
#include "KHParse.h"
#include "KHRadioLayout.h"
#include "KHWebSession.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <thread>

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

int wmain(int argc, wchar_t** argv)
{
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
	TestSession();
	TestLayout();
	TestHistory(dir);
	TestAudioCache(dir);
	TestOggDecode(dir);


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
