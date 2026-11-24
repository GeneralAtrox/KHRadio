/*
 * KH Radio - the talking-track detector: decodes short windows of a track and
 * decides whether it is people talking (a drama or cutscene track) rather than
 * music. Pure DSP, no network and no UI, so the tests drive it with generated
 * audio and the evaluation mode with real tracks.
 *
 * Copyright (C) 2026 KH Radio authors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this
 * program (COPYING.txt). If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <atlstr.h>
#include <functional>
#include <vector>
#include "KHParse.h"
#include "RadioTypes.h"

namespace KHAudio
{
	// Bytes fetched per window of an mp3 (about 10-15 s at common bit rates).
	constexpr size_t kWindowBytes = 262144;
	// A window this quiet everywhere (loudest 20 ms RMS, full scale = 1) is silent.
	constexpr double kQuietPeak = 0.015;

	// What one window of audio looks like.
	struct Features {
		bool valid = false;    // at least ~1 s of audio was there to judge
		bool quiet = false;    // never louder than kQuietPeak
		double seconds = 0.0;
		double peak = 0.0;     // loudest 20 ms RMS
		// the 1.3 classifier's measures (kept for comparison in the log)
		double flux = 0.0;     // mean spectral change per 23 ms
		double pause = 0.0;    // share of 20 ms frames below a quarter of the mean level
		double zcrCoV = 0.0;   // variation of the zero-crossing rate
		// measures added in 1.4
		double tonal = 0.0;    // share of spectral-peak energy held at a steady pitch for >= 250 ms
		double mod4 = 0.0;     // share of loudness modulation at 2.5-7 Hz (syllable rate)
		double envStd = 0.0;   // spread of the 300-3000 Hz loudness, dB
		double beat = 0.0;     // strongest repeat of the onsets at a 0.25-1.5 s period (0..1)
		double centroidCoV = 0.0; // variation of the spectral centre of mass
		double gaps = 0.0;     // share of 12 ms frames 25 dB under the loud level
		double score = 0.0;    // how much this window sounds like talking (see SpeechScore)
	};

	Features Analyse(const float* mono, size_t count, int hz);
	inline Features Analyse(const std::vector<float>& mono, int hz) { return Analyse(mono.data(), mono.size(), hz); }

	// The 1.3 rule on one window: flux < 10 and (zcrCoV > 0.70 or pause > 0.30).
	bool OldRuleSpoken(const Features& f);

	// Talking score of one window: about -4..+4, higher = more like talking.
	double SpeechScore(const Features& f);

	// The track's score is its lowest window score; it is talking of this strength
	// at or above these.
	enum class Strength { None, Weak, Moderate, Strong, VeryStrong };
	constexpr double kWeakScore = 0.0;       // enough only when the title says drama / scene / message too
	constexpr double kModerateScore = 1.0;   // enough in an album that is mostly talking
	constexpr double kStrongScore = 2.5;     // enough on its own
	constexpr double kVeryStrongScore = 3.0; // needed in an album that has sounded musical

	// A long track is also sampled along its whole length, because a talk show
	// that alternates talking with music can have music at both of the first
	// two windows. Tracks of kSpreadMinBytes or more get kSpreadWindows shorter
	// windows, one in the middle of each equal slice of the file; the track is
	// talking when a quarter or more of them score kModerateScore or higher.
	constexpr size_t kSpreadWindowBytes = 131072;
	constexpr unsigned long long kSpreadMinBytes = 8ull * 1024 * 1024;
	constexpr int kSpreadWindows = 16;
	constexpr int kSpreadMinJudged = 8; // fewer windows than this decide nothing

	struct Decision {
		KHInsider::AudioVerdict verdict = KHInsider::AudioVerdict::Music;
		Strength strength = Strength::None; // for Spoken
		double score = 0.0;    // the windows' combined talking score
		int windows = 0;       // windows that could be judged
		Features w[2];
		bool judged = false;   // false: no audio could be analysed (counts as music)
		int spreadJudged = 0;  // windows judged along a long track (0: not sampled)
		int spreadTalking = 0; // ... of which scored kModerateScore or higher
		bool spreadTalk = false; // talking at a quarter or more of those windows
	};

	// Combines the windows: talking only when every window says so; silent only
	// when every window is silent.
	Decision Decide(const Features* w, int count);
	// Adds the windows sampled along a long track to a decision. Enough talking
	// among them makes the track Spoken whatever the first two windows said.
	void ApplySpread(Decision& d, const Features* w, int count);
	// Where spread window 'index' (0-based) starts, or 0 when the file is too short.
	unsigned long long SpreadWindowStart(unsigned long long total, unsigned long long audioStart, int index);
	Decision ClassifyPcmWindows(const std::vector<float>& first, const std::vector<float>& middle, int hz);

	// Mp3 frames to mono floats (-1..1); 'skipFrames' decoded frames are dropped
	// first (a window cut from the middle of a stream starts without its bit
	// reservoir). At most maxSeconds are kept.
	bool DecodeMp3(const unsigned char* data, size_t len, int skipFrames, double maxSeconds, std::vector<float>& mono, int& hz);

	// One ranged GET: bytes from..to (inclusive). 'total' is the file size from
	// a 206 reply's Content-Range, or 0 if the server sent no range.
	struct RangeReply {
		std::vector<char> data;
		unsigned long long total = 0;
		bool partial = false;
	};
	using RangeFetch = std::function<bool(unsigned long long from, unsigned long long to, RangeReply& reply)>;

	// The mp3 route: the first window after any ID3 tag, then one around the
	// middle of the file (placed from the size the first reply reported).
	Decision ClassifyMp3(const RangeFetch& fetch, size_t* bytesFetched = nullptr);

	// Where the middle window starts, or 0 when the file is too short for one.
	unsigned long long MiddleWindowStart(unsigned long long total, unsigned long long audioStart, size_t windowBytes);

	// One log line's worth: every measure of every window, the score and verdict.
	CStringW Describe(const Decision& d);
	LPCWSTR StrengthName(Strength s);

	// What the radio has heard of one album so far: tracks whose audio was
	// judged, and how many of those sounded like talking (Moderate or more).
	struct AlbumTally {
		int judged = 0;
		int talking = 0;
		bool MostlyTalking() const { return judged >= 2 && talking * 2 > judged; }
		bool Musical() const { return judged - talking >= 3 && talking <= 2; }
		void Add(const Decision& d);
	};

	// The filter's call on one track: the audio decision, the track title's cue
	// and the album so far. Talking needs strong evidence in a musical album and
	// less in an album that is mostly talking or when the title agrees.
	struct FilterCall {
		SkipReason skip = SkipReason::None;
		LPCWSTR rule = L"";  // which rule decided, for the log
	};
	FilterCall ApplyAlbumContext(const Decision& d, KHInsider::TitleCue cue, const AlbumTally& album);
}
