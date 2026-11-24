// KH Radio - the talking-track detector (see AudioClassify.h).
// GPL-3.0-or-later (see COPYING.txt).
#include "stdafx.h"
#include "AudioClassify.h"

#pragma warning(push, 0)
#pragma warning(disable: 4146 4244 4267 4456 4457 4701 4703)
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3/minimp3.h"
#pragma warning(pop)

#include <cmath>
#include <numeric>

namespace KHAudio
{
	namespace
	{
		constexpr double kPi = 3.14159265358979323846;

		// In-place radix-2 FFT (n a power of two).
		void FFT(double* re, double* im, int n)
		{
			for (int i = 1, j = 0; i < n; i++) {
				int bit = n >> 1;
				for (; j & bit; bit >>= 1) { j ^= bit; }
				j ^= bit;
				if (i < j) {
					std::swap(re[i], re[j]);
					std::swap(im[i], im[j]);
				}
			}
			for (int len = 2; len <= n; len <<= 1) {
				const double ang = -2.0 * kPi / len;
				const double wr = std::cos(ang), wi = std::sin(ang);
				for (int i = 0; i < n; i += len) {
					double cr = 1.0, ci = 0.0;
					for (int k = 0; k < len / 2; k++) {
						const double pr = re[i + k], pim = im[i + k];
						const double qr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
						const double qi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
						re[i + k] = pr + qr; im[i + k] = pim + qi;
						re[i + k + len / 2] = pr - qr; im[i + k + len / 2] = pim - qi;
						const double ncr = cr * wr - ci * wi;
						ci = cr * wi + ci * wr; cr = ncr;
					}
				}
			}
		}

		std::vector<double> Hann(int n)
		{
			std::vector<double> w(static_cast<size_t>(n));
			for (int i = 0; i < n; i++) {
				w[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
			}
			return w;
		}

		// The 1.3 measure, unchanged: mean frame-to-frame change of the magnitude
		// spectrum (1024-sample frames, hop 512).
		double SpectralFlux(const float* x, size_t count)
		{
			constexpr int N = 1024, HOP = 512, BINS = N / 2 + 1;
			if (count < static_cast<size_t>(N) * 4) {
				return 0.0;
			}
			const auto hann = Hann(N);
			std::vector<double> re(N), im(N), mag(BINS), prev(BINS, 0.0);
			double sum = 0.0;
			int frames = 0;
			bool havePrev = false;
			for (size_t s = 0; s + N <= count; s += HOP) {
				for (int i = 0; i < N; i++) { re[i] = static_cast<double>(x[s + i]) * hann[i]; im[i] = 0.0; }
				FFT(re.data(), im.data(), N);
				for (int k = 0; k < BINS; k++) { mag[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]); }
				if (havePrev) {
					double d2 = 0.0;
					for (int k = 0; k < BINS; k++) { const double d = mag[k] - prev[k]; d2 += d * d; }
					sum += std::sqrt(d2);
					frames++;
				}
				prev = mag;
				havePrev = true;
			}
			return frames > 0 ? sum / frames : 0.0;
		}

		double Percentile(std::vector<double> v, double p)
		{
			if (v.empty()) {
				return 0.0;
			}
			const size_t at = std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5));
			std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(at), v.end());
			return v[at];
		}

		// A spectral peak followed from frame to frame while its pitch holds.
		struct Run {
			double meanBin = 0.0;
			int start = 0;
			int last = 0;
			int count = 0;
			double sum = 0.0;
			int touched = -1; // frame it was last extended in
		};

		// The steady-pitch, loudness-modulation, beat and spectral-centre measures.
		void SpectralMeasures(const float* x, size_t count, int hz, Features& f)
		{
			int N = 256;
			while (N < hz * 0.09) {
				N <<= 1; // ~93 ms at 44.1 kHz: fine enough to see a held note's pitch
			}
			const int hop = N / 8;
			if (count < static_cast<size_t>(N) + static_cast<size_t>(hop) * 40) {
				return;
			}
			const int T = static_cast<int>((count - N) / hop) + 1;
			const double binHz = static_cast<double>(hz) / N;
			const double hopSec = static_cast<double>(hop) / hz;
			const int bins = N / 2 + 1;
			const auto bin = [&](double fHz) { return std::clamp(static_cast<int>(fHz / binHz + 0.5), 1, bins - 2); };
			const int kBand0 = bin(300), kBand1 = bin(3000), kLo = bin(150), kHi = bin(4000), kAll0 = bin(50), kAll1 = bin(8000);
			const double norm = 4.0 / N; // a full-scale sine peaks near 1

			const auto hann = Hann(N);
			std::vector<double> re(static_cast<size_t>(N)), im(static_cast<size_t>(N)), mag(static_cast<size_t>(bins)), logPrev(static_cast<size_t>(bins), 0.0);
			std::vector<double> bandDb(static_cast<size_t>(T)), allDb(static_cast<size_t>(T)), centroid(static_cast<size_t>(T)), onset(static_cast<size_t>(T), 0.0);
			std::vector<std::vector<std::pair<int, double>>> peaks(static_cast<size_t>(T));

			for (int t = 0; t < T; t++) {
				const float* s = x + static_cast<size_t>(t) * hop;
				for (int i = 0; i < N; i++) { re[i] = static_cast<double>(s[i]) * hann[i]; im[i] = 0.0; }
				FFT(re.data(), im.data(), N);
				for (int k = 0; k < bins; k++) { mag[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]) * norm; }
				double band = 0.0, all = 0.0, cw = 0.0, cs = 0.0, on = 0.0, peakMax = 0.0;
				for (int k = kAll0; k <= kAll1; k++) {
					const double p = mag[k] * mag[k];
					all += p;
					if (k >= kBand0 && k <= kBand1) { band += p; }
					cw += k * binHz * mag[k];
					cs += mag[k];
					const double l = std::log1p(100.0 * mag[k]);
					if (t > 0) { on += std::max(0.0, l - logPrev[k]); }
					logPrev[k] = l;
					if (k >= kLo && k <= kHi) { peakMax = std::max(peakMax, mag[k]); }
				}
				bandDb[t] = 10.0 * std::log10(band + 1e-12);
				allDb[t] = 10.0 * std::log10(all + 1e-12);
				centroid[t] = cs > 0.0 ? cw / cs : 0.0;
				onset[t] = on / (kAll1 - kAll0 + 1);
				auto& list = peaks[static_cast<size_t>(t)];
				for (int k = std::max(kLo, 2); k <= std::min(kHi, bins - 3); k++) {
					const double m = mag[k];
					if (m > peakMax * 0.05 && m > 1e-4 && m > mag[k - 1] && m >= mag[k + 1] && m > mag[k - 2] && m >= mag[k + 2]) {
						list.emplace_back(k, m);
					}
				}
				std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
				if (list.size() > 16) {
					list.resize(16);
				}
			}

			const double loudAll = Percentile(allDb, 0.95);
			const double loudBand = Percentile(bandDb, 0.90);

			// steady pitch: spectral peaks that stay within ~3% (at least 1.5 bins)
			// of their mean frequency for 250 ms or more
			std::vector<Run> active;
			double stable = 0.0, total = 0.0;
			const auto close = [&](const Run& r) {
				total += r.sum;
				if ((r.last - r.start + 1) * hopSec >= 0.25) {
					stable += r.sum;
				}
			};
			for (int t = 0; t < T; t++) {
				if (allDb[t] < loudAll - 45.0) {
					continue; // silence: nothing to follow
				}
				for (const auto& [k, m] : peaks[static_cast<size_t>(t)]) {
					Run* best = nullptr;
					double bestDist = 1e9;
					for (auto& r : active) {
						const double dist = std::fabs(k - r.meanBin);
						if (r.touched != t && r.last >= t - 2 && dist <= std::max(1.5, 0.03 * r.meanBin) && dist < bestDist) {
							best = &r;
							bestDist = dist;
						}
					}
					if (best) {
						best->meanBin = (best->meanBin * best->count + k) / (best->count + 1);
						best->count++;
						best->last = t;
						best->sum += m;
						best->touched = t;
					} else {
						Run r;
						r.meanBin = k;
						r.start = r.last = r.touched = t;
						r.count = 1;
						r.sum = m;
						active.push_back(r);
					}
				}
				for (auto it = active.begin(); it != active.end();) {
					if (it->last < t - 2) {
						close(*it);
						it = active.erase(it);
					} else {
						++it;
					}
				}
			}
			for (const auto& r : active) {
				close(r);
			}
			f.tonal = total > 0.0 ? stable / total : 0.0;

			// loudness modulation of the speech band: syllables come 3-6 times a second
			std::vector<double> env(bandDb);
			double mean = 0.0;
			for (auto& e : env) {
				e = std::max(e, loudBand - 45.0);
				mean += e;
			}
			mean /= T;
			double var = 0.0;
			for (auto& e : env) {
				e -= mean;
				var += e * e;
			}
			f.envStd = std::sqrt(var / T);
			const auto envWin = Hann(T);
			const double frameRate = 1.0 / hopSec;
			double inBand = 0.0, allMod = 0.0;
			for (double fm = 0.5; fm <= 20.0001; fm += 0.25) {
				double a = 0.0, b = 0.0;
				const double w = 2.0 * kPi * fm / frameRate;
				for (int t = 0; t < T; t++) {
					a += env[t] * envWin[t] * std::cos(w * t);
					b += env[t] * envWin[t] * std::sin(w * t);
				}
				const double p = a * a + b * b;
				allMod += p;
				if (fm >= 2.5 && fm <= 7.0) {
					inBand += p;
				}
			}
			f.mod4 = allMod > 0.0 ? inBand / allMod : 0.0;

			// short gaps between phrases
			int gaps = 0;
			for (int t = 0; t < T; t++) {
				if (bandDb[t] < loudBand - 25.0) {
					gaps++;
				}
			}
			f.gaps = static_cast<double>(gaps) / T;

			// spectral centre of mass: voiced sounds and hisses alternate in speech
			double cSum = 0.0, cSq = 0.0;
			int cN = 0;
			for (int t = 0; t < T; t++) {
				if (allDb[t] >= loudAll - 30.0) {
					cSum += centroid[t];
					cSq += centroid[t] * centroid[t];
					cN++;
				}
			}
			if (cN > 10 && cSum > 0.0) {
				const double cm = cSum / cN;
				f.centroidCoV = std::sqrt(std::max(0.0, cSq / cN - cm * cm)) / cm;
			}

			// beat: the onsets repeat at a steady period
			const int half = std::max(1, static_cast<int>(0.25 / hopSec));
			std::vector<double> o(static_cast<size_t>(T), 0.0);
			std::vector<double> prefix(static_cast<size_t>(T) + 1, 0.0);
			for (int t = 0; t < T; t++) {
				prefix[t + 1] = prefix[t] + onset[t];
			}
			for (int t = 0; t < T; t++) {
				const int a = std::max(0, t - half), b = std::min(T, t + half + 1);
				o[t] = std::max(0.0, onset[t] - (prefix[b] - prefix[a]) / (b - a));
			}
			double r0 = 0.0;
			for (int t = 0; t < T; t++) {
				r0 += o[t] * o[t];
			}
			const int lag0 = std::max(1, static_cast<int>(0.25 / hopSec)), lag1 = std::min(T / 2, static_cast<int>(1.5 / hopSec));
			double best = 0.0;
			if (r0 > 0.0) {
				for (int lag = lag0; lag <= lag1; lag++) {
					double r = 0.0;
					for (int t = 0; t + lag < T; t++) {
						r += o[t] * o[t + lag];
					}
					best = std::max(best, r / r0 * T / (T - lag));
				}
			}
			f.beat = std::min(1.0, best);
		}

		LPCWSTR VerdictName(const Decision& d)
		{
			switch (d.verdict) {
				case KHInsider::AudioVerdict::Spoken: return L"TALKING";
				case KHInsider::AudioVerdict::Quiet: return L"SILENT";
				default: return L"music";
			}
		}
	}

	Features Analyse(const float* x, size_t count, int hz)
	{
		Features f;
		if (!x || hz <= 0 || count < static_cast<size_t>(hz)) {
			return f; // under a second: nothing to judge
		}
		const int frameLen = hz / 50;
		const int nFrames = frameLen > 0 ? static_cast<int>(count / static_cast<size_t>(frameLen)) : 0;
		if (frameLen < 1 || nFrames < 25) {
			return f;
		}
		f.valid = true;
		f.seconds = static_cast<double>(count) / hz;

		// 20 ms frames: level and zero-crossing rate (as in 1.3)
		std::vector<float> energy(static_cast<size_t>(nFrames)), zcr(static_cast<size_t>(nFrames));
		double maxE = 0.0, meanE = 0.0;
		for (int fr = 0; fr < nFrames; fr++) {
			const float* s = x + static_cast<size_t>(fr) * frameLen;
			double e = 0.0;
			int zc = 0;
			for (int i = 0; i < frameLen; i++) {
				e += static_cast<double>(s[i]) * s[i];
				if (i > 0 && ((s[i] >= 0.0f) != (s[i - 1] >= 0.0f))) {
					zc++;
				}
			}
			energy[fr] = static_cast<float>(std::sqrt(e / frameLen));
			zcr[fr] = static_cast<float>(zc) / frameLen;
			maxE = std::max<double>(maxE, energy[fr]);
			meanE += energy[fr];
		}
		meanE /= nFrames;
		f.peak = maxE;
		if (maxE < kQuietPeak) {
			f.quiet = true;
			return f;
		}
		int pauses = 0;
		double meanZ = 0.0;
		for (int fr = 0; fr < nFrames; fr++) {
			pauses += energy[fr] < 0.25 * meanE ? 1 : 0;
			meanZ += zcr[fr];
		}
		meanZ /= nFrames;
		double varZ = 0.0;
		for (const float z : zcr) {
			varZ += (z - meanZ) * (z - meanZ);
		}
		varZ /= nFrames;
		f.pause = static_cast<double>(pauses) / nFrames;
		f.zcrCoV = meanZ > 1e-6 ? std::sqrt(varZ) / meanZ : 0.0;
		f.flux = SpectralFlux(x, count);
		SpectralMeasures(x, count, hz, f);
		f.score = SpeechScore(f);
		return f;
	}

	bool OldRuleSpoken(const Features& f)
	{
		return f.valid && !f.quiet && f.flux < 10.0 && (f.zcrCoV > 0.70 || f.pause > 0.30);
	}

	// A linear score fitted (logistic regression) on half of a labelled sample of
	// game-soundtrack tracks - drama / cutscene / radio-show tracks against game
	// music of every kind - and checked on the other half. Music holds steady
	// pitches (tonal) and repeats its onsets (beat); talking has neither, pauses
	// between phrases, and a loudness that swings more.
	double SpeechScore(const Features& f)
	{
		if (!f.valid || f.quiet) {
			return -1.0;
		}
		return 6.68
			- 3.50 * f.tonal
			- 7.47 * f.beat
			+ 0.66 * f.mod4
			+ 0.035 * f.envStd
			- 4.39 * f.gaps
			+ 3.08 * f.pause
			+ 0.10 * f.centroidCoV
			- 0.86 * std::log1p(f.flux)
			- 0.27 * f.zcrCoV;
	}

	Decision Decide(const Features* w, int count)
	{
		Decision d;
		int n = 0;
		for (int i = 0; i < count && n < 2; i++) {
			if (w[i].valid) {
				d.w[n++] = w[i];
			}
		}
		d.windows = n;
		d.judged = n > 0;
		if (!n) {
			return d;
		}
		bool allQuiet = true;
		double score = 1e9;
		for (int i = 0; i < n; i++) {
			allQuiet = allQuiet && d.w[i].quiet;
			score = std::min(score, d.w[i].score);
		}
		d.score = score;
		if (allQuiet) {
			d.verdict = KHInsider::AudioVerdict::Quiet;
			return d;
		}
		// Every window must sound like talking (a spoken intro before the music is
		// kept). The thresholds were set so that no music in the sample would have
		// been dropped with the album context applied (see the README).
		if (score >= kVeryStrongScore) {
			d.strength = Strength::VeryStrong;
		} else if (score >= kStrongScore) {
			d.strength = Strength::Strong;
		} else if (score >= kModerateScore) {
			d.strength = Strength::Moderate;
		} else if (score >= kWeakScore) {
			d.strength = Strength::Weak;
		}
		if (d.strength != Strength::None) {
			d.verdict = KHInsider::AudioVerdict::Spoken;
		}
		return d;
	}

	void ApplySpread(Decision& d, const Features* w, int count)
	{
		d.spreadJudged = d.spreadTalking = 0;
		d.spreadTalk = false;
		for (int i = 0; i < count; i++) {
			if (!w[i].valid) {
				continue;
			}
			d.spreadJudged++;
			if (!w[i].quiet && w[i].score >= kModerateScore) {
				d.spreadTalking++;
			}
		}
		if (d.spreadJudged >= kSpreadMinJudged && d.spreadTalking * 4 >= d.spreadJudged) {
			d.spreadTalk = true;
			d.judged = true;
			d.verdict = KHInsider::AudioVerdict::Spoken;
			d.strength = std::max(d.strength, Strength::Moderate);
		}
	}

	unsigned long long SpreadWindowStart(unsigned long long total, unsigned long long audioStart, int index)
	{
		if (index < 0 || index >= kSpreadWindows || total <= audioStart || total - audioStart < kSpreadMinBytes) {
			return 0;
		}
		const unsigned long long slice = (total - audioStart) / kSpreadWindows;
		return audioStart + slice * static_cast<unsigned long long>(index) + slice / 2 - kSpreadWindowBytes / 2;
	}

	Decision ClassifyPcmWindows(const std::vector<float>& first, const std::vector<float>& middle, int hz)
	{
		Features w[2] = { Analyse(first, hz), Analyse(middle, hz) };
		return Decide(w, 2);
	}

	bool DecodeMp3(const unsigned char* data, size_t len, int skipFrames, double maxSeconds, std::vector<float>& mono, int& hz)
	{
		mono.clear();
		hz = 0;
		if (!data || len < 4) {
			return false;
		}
		mp3dec_t dec;
		mp3dec_init(&dec);
		mp3dec_frame_info_t info = {};
		std::vector<short> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
		const uint8_t* p = data;
		int left = static_cast<int>(std::min<size_t>(len, 64u * 1024 * 1024));
		int decoded = 0;
		while (left > 0) {
			const int samples = mp3dec_decode_frame(&dec, p, left, pcm.data(), &info);
			if (info.frame_bytes <= 0) {
				break;
			}
			p += info.frame_bytes;
			left -= info.frame_bytes;
			if (samples <= 0 || (hz && info.hz != hz)) {
				continue; // a header without audio, or a false sync inside the stream
			}
			hz = info.hz;
			if (decoded++ < skipFrames) {
				continue;
			}
			if (mono.size() >= static_cast<size_t>(maxSeconds * hz)) {
				break;
			}
			if (info.channels >= 2) {
				for (int i = 0; i < samples; i++) {
					mono.push_back((pcm[2 * i] + pcm[2 * i + 1]) * (0.5f / 32768.0f));
				}
			} else {
				for (int i = 0; i < samples; i++) {
					mono.push_back(pcm[i] / 32768.0f);
				}
			}
		}
		return hz > 0 && !mono.empty();
	}

	unsigned long long MiddleWindowStart(unsigned long long total, unsigned long long audioStart, size_t windowBytes)
	{
		if (total <= audioStart || total - audioStart < 2ull * windowBytes) {
			return 0; // the first window already covers half the track or more
		}
		return audioStart + (total - audioStart) / 2 - windowBytes / 2;
	}

	Decision ClassifyMp3(const RangeFetch& fetch, size_t* bytesFetched)
	{
		size_t bytes = 0;
		const auto getBytes = [&](unsigned long long from, size_t want, RangeReply& reply) {
			const bool ok = fetch(from, from + want - 1, reply);
			bytes += reply.data.size();
			return ok && reply.data.size() >= 8192;
		};
		const auto get = [&](unsigned long long from, RangeReply& reply) { return getBytes(from, kWindowBytes, reply); };
		Features w[2];
		int n = 0;
		unsigned long long total = 0, audioStart = 0;
		RangeReply first;
		if (get(0, first)) {
			total = first.partial ? first.total : 0;
			const auto* u = reinterpret_cast<const unsigned char*>(first.data.data());
			size_t offset = KHInsider::Id3v2TagSize(u, first.data.size());
			audioStart = offset;
			bool have = true;
			if (offset > 0 && offset + 65536 > first.data.size()) {
				// a big ID3 tag (embedded cover art) fills the window: read after it
				have = offset <= 32u * 1024 * 1024 && first.partial && get(offset, first);
				if (have && first.partial) {
					total = first.total;
				}
				u = reinterpret_cast<const unsigned char*>(first.data.data());
				offset = 0;
			}
			std::vector<float> mono;
			int hz = 0;
			if (have && DecodeMp3(u + offset, first.data.size() - offset, 0, 30.0, mono, hz)) {
				w[n++] = Analyse(mono, hz);
			}
			const unsigned long long mid = n ? MiddleWindowStart(total, audioStart, kWindowBytes) : 0;
			RangeReply middle;
			if (mid && get(mid, middle) && middle.partial
					&& DecodeMp3(reinterpret_cast<const unsigned char*>(middle.data.data()), middle.data.size(), 2, 30.0, mono, hz)) {
				w[n++] = Analyse(mono, hz);
			}
		}
		Decision d = Decide(w, n);
		// A long track the first two windows did not call talking: sample it along
		// its length (see kSpreadWindows).
		if (n && d.verdict == KHInsider::AudioVerdict::Music && SpreadWindowStart(total, audioStart, 0)) {
			Features s[kSpreadWindows];
			int failed = 0;
			for (int i = 0; i < kSpreadWindows && failed < 3; i++) {
				RangeReply part;
				std::vector<float> mono;
				int hz = 0;
				if (getBytes(SpreadWindowStart(total, audioStart, i), kSpreadWindowBytes, part) && part.partial
						&& DecodeMp3(reinterpret_cast<const unsigned char*>(part.data.data()), part.data.size(), 2, 30.0, mono, hz)) {
					s[i] = Analyse(mono, hz);
				} else {
					failed++;
				}
			}
			ApplySpread(d, s, kSpreadWindows);
		}
		if (bytesFetched) {
			*bytesFetched = bytes;
		}
		return d;
	}

	void AlbumTally::Add(const Decision& d)
	{
		if (!d.judged || d.verdict == KHInsider::AudioVerdict::Quiet) {
			return;
		}
		judged++;
		if (d.verdict == KHInsider::AudioVerdict::Spoken && d.strength >= Strength::Moderate) {
			talking++;
		}
	}

	FilterCall ApplyAlbumContext(const Decision& d, KHInsider::TitleCue cue, const AlbumTally& album)
	{
		if (!d.judged) {
			return { SkipReason::None, L"not judged" };
		}
		if (d.verdict == KHInsider::AudioVerdict::Quiet) {
			return { SkipReason::Silent, L"silent in every window" };
		}
		if (d.spreadTalk) {
			return { SkipReason::Talking, L"talking at a quarter or more of the points along a long track" };
		}
		if (d.verdict != KHInsider::AudioVerdict::Spoken || d.strength == Strength::None) {
			return { SkipReason::None, L"music" };
		}
		if (cue != KHInsider::TitleCue::None) {
			return { SkipReason::Talking, L"title cue + talking audio" };
		}
		if (d.strength < Strength::Moderate) {
			return { SkipReason::None, L"talking too weak without a title cue" };
		}
		if (album.MostlyTalking()) {
			return { SkipReason::Talking, L"album mostly talking + talking audio" };
		}
		if (album.Musical()) {
			return d.strength >= Strength::VeryStrong ? FilterCall{ SkipReason::Talking, L"very strong talking in a musical album" }
				: FilterCall{ SkipReason::None, L"musical album: talking not strong enough" };
		}
		return d.strength >= Strength::Strong ? FilterCall{ SkipReason::Talking, L"strong talking" }
			: FilterCall{ SkipReason::None, L"talking not strong enough" };
	}

	LPCWSTR StrengthName(Strength s)
	{
		switch (s) {
			case Strength::Weak: return L"weak";
			case Strength::Moderate: return L"moderate";
			case Strength::Strong: return L"strong";
			case Strength::VeryStrong: return L"very strong";
			default: return L"none";
		}
	}

	CStringW Describe(const Decision& d)
	{
		CStringW s;
		if (!d.judged) {
			return L"no audio could be analysed -> music";
		}
		for (int i = 0; i < d.windows; i++) {
			const Features& f = d.w[i];
			if (f.quiet) {
				s.AppendFormat(L"w%d[%.0fs silent peak=%.4f] ", i + 1, f.seconds, f.peak);
			} else {
				s.AppendFormat(L"w%d[%.0fs peak=%.3f flux=%.1f pause=%.2f zcrCoV=%.2f tonal=%.2f mod4=%.2f env=%.1f beat=%.2f cent=%.2f gaps=%.2f s=%+.2f] ",
					i + 1, f.seconds, f.peak, f.flux, f.pause, f.zcrCoV, f.tonal, f.mod4, f.envStd, f.beat, f.centroidCoV, f.gaps, f.score);
			}
		}
		if (d.spreadJudged) {
			s.AppendFormat(L"along the track %d/%d talking ", d.spreadTalking, d.spreadJudged);
		}
		s.AppendFormat(L"score=%+.2f -> %s", d.score, VerdictName(d));
		if (d.verdict == KHInsider::AudioVerdict::Spoken) {
			s.AppendFormat(L" (%s)", StrengthName(d.strength));
		}
		return s;
	}
}
