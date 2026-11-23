/*
 * KH Radio - pure parsing for downloads.khinsider.com pages (no network, no UI).
 *
 * Copyright (C) 2026 KH Radio authors.
 * Derived from the KH Radio panel of the MPC-BE fork (MPC-BE, GPLv3).
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

#include "stdafx.h"
#include "KHParse.h"
#include "TextUtil.h"

#include <map>
#include <regex>

#define KHINSIDER_BASE L"https://downloads.khinsider.com"

namespace KHInsider
{
	namespace
	{
		// The patterns live for the whole process (never destroyed), so a worker
		// thread still parsing while the app exits can't touch a dead regex.
		const std::regex& Re(const char* pattern, bool icase)
		{
			return *new std::regex(pattern, icase ? (std::regex::ECMAScript | std::regex::icase) : std::regex::ECMAScript);
		}

		CStringW StripTags(const CStringW& str)
		{
			CStringW out;
			bool inTag = false;
			for (int i = 0; i < str.GetLength(); i++) {
				const WCHAR ch = str[i];
				if (ch == L'<') {
					inTag = true;
				} else if (ch == L'>') {
					inTag = false;
				} else if (!inTag) {
					out += ch;
				}
			}
			return out;
		}

		CStringW Trimmed(CStringW str)
		{
			str.Trim(L" \t\r\n");
			return str;
		}

		CStringW FromUtf8(const std::string& s)
		{
			return UTF8ToWStr(s.c_str());
		}
	}

	CStringW DecodeHtmlEntities(CStringW str)
	{
		str.Replace(L"&quot;", L"\"");
		str.Replace(L"&#34;", L"\"");
		str.Replace(L"&#39;", L"'");
		str.Replace(L"&apos;", L"'");
		str.Replace(L"&lt;", L"<");
		str.Replace(L"&gt;", L">");
		str.Replace(L"&nbsp;", L" ");

		// numeric entities: &#1234; and &#x12AB;
		int pos = 0;
		while ((pos = str.Find(L"&#", pos)) != -1) {
			const int semi = str.Find(L';', pos);
			if (semi == -1 || semi - pos > 9) {
				pos += 2;
				continue;
			}
			const CStringW num = str.Mid(pos + 2, semi - pos - 2);
			WCHAR* endPtr = nullptr;
			long code = 0;
			const CStringW hex = num.Mid(1); // endPtr points into it, so it must outlive the check below
			if (!num.IsEmpty() && (num[0] == L'x' || num[0] == L'X')) {
				code = wcstol(hex, &endPtr, 16);
			} else {
				code = wcstol(num, &endPtr, 10);
			}
			if (code > 0 && code < 0x10000 && endPtr && *endPtr == 0) {
				str.Delete(pos, semi - pos + 1);
				str.Insert(pos, static_cast<WCHAR>(code));
				pos++;
			} else {
				pos += 2;
			}
		}

		str.Replace(L"&amp;", L"&"); // last, so "&amp;quot;" doesn't double-decode
		return str;
	}

	bool ParseAlbumPage(const std::string& html, const CStringW& pageUrl, Album& album, bool* usedFallback)
	{
		album = Album();
		album.url = pageUrl;
		if (usedFallback) {
			*usedFallback = false;
		}

		// Album title: first <h2> inside the page content.
		size_t contentPos = html.find("id=\"pageContent\"");
		if (contentPos == std::string::npos) {
			contentPos = 0;
		}
		const size_t h2Start = html.find("<h2>", contentPos);
		if (h2Start != std::string::npos) {
			const size_t h2End = html.find("</h2>", h2Start);
			if (h2End != std::string::npos) {
				album.title = Trimmed(DecodeHtmlEntities(StripTags(FromUtf8(html.substr(h2Start + 4, h2End - h2Start - 4)))));
			}
		}

		// Track list: links inside the songlist table, skipping the
		// header/footer rows (their cells may hold sort/download links).
		size_t listStart = html.find("id=\"songlist\"");
		size_t listEnd = html.size();
		bool bFallback = false;
		if (listStart != std::string::npos) {
			const size_t headerPos = html.find("id=\"songlist_header\"", listStart);
			if (headerPos != std::string::npos) {
				const size_t headerEnd = html.find("</tr>", headerPos);
				if (headerEnd != std::string::npos) {
					listStart = headerEnd;
				}
			}
			listEnd = html.find("id=\"songlist_footer\"", listStart);
			if (listEnd == std::string::npos) {
				listEnd = html.find("</table>", listStart);
			}
			if (listEnd == std::string::npos) {
				listEnd = html.size();
			}
		} else {
			// The songlist marker is gone - the site may have been redesigned.
			// Scan the whole page, but only accept links under THIS album's
			// path so we don't pick up sidebar links to other albums.
			bFallback = true;
			listStart = 0;
		}
		if (usedFallback) {
			*usedFallback = bFallback;
		}
		const std::string songlist = html.substr(listStart, listEnd - listStart);

		// album path prefix, e.g. "/game-soundtracks/album/<slug>/", for the fallback
		std::string albumPrefix;
		{
			const CStringA puA = WStrToUTF8(pageUrl);
			const std::string pu(puA.GetString());
			const size_t ap = pu.find("/game-soundtracks/album/");
			if (ap != std::string::npos) {
				albumPrefix = pu.substr(ap) + "/";
			}
		}

		static const std::regex& reLink = Re("<a\\s+href=\"(/game-soundtracks/album/[^\"]+)\"[^>]*>([^<]+)</a>", true);
		static const std::regex& reDuration = Re("^\\d+:\\d{2}(:\\d{2})?$", false);
		static const std::regex& reSize = Re("^[\\d.,]+\\s*[KMG]B$", true);

		std::map<std::string, bool> seen;
		std::string lastHref; // href of the most recently added track (for its duration cell)

		for (auto it = std::sregex_iterator(songlist.begin(), songlist.end(), reLink); it != std::sregex_iterator(); ++it) {
			const std::string href = (*it)[1].str();
			const std::string text = (*it)[2].str();

			// in fallback mode keep only this album's own track links
			if (bFallback && (albumPrefix.empty() || href.compare(0, albumPrefix.size(), albumPrefix) != 0)) {
				continue;
			}

			const CStringW name = Trimmed(DecodeHtmlEntities(FromUtf8(text)));
			if (name.IsEmpty()) {
				continue;
			}
			const std::string nameUtf8(WStrToUTF8(name));
			if (std::regex_match(nameUtf8, reDuration)) {
				// each row has a duration cell linking to the same track page;
				// record it against the track we just added
				if (!album.tracks.empty() && href == lastHref && album.tracks.back().durationSec == 0) {
					int a = 0, b = 0, c = 0;
					const int parts = sscanf_s(nameUtf8.c_str(), "%d:%d:%d", &a, &b, &c);
					album.tracks.back().durationSec = (parts == 3) ? (a * 3600 + b * 60 + c)
						: (parts == 2) ? (a * 60 + b) : 0;
				}
				continue;
			}
			if (std::regex_match(nameUtf8, reSize)) {
				continue;
			}
			if (seen.find(href) != seen.end()) {
				continue;
			}
			seen[href] = true;

			Track track;
			track.name = name;
			track.pageUrl = KHINSIDER_BASE + DecodeHtmlEntities(FromUtf8(href));
			track.number = static_cast<int>(album.tracks.size()) + 1;
			album.tracks.emplace_back(track);
			lastHref = href;
		}

		if (album.title.IsEmpty() && !pageUrl.IsEmpty()) {
			// fall back to the URL slug
			const int slash = pageUrl.ReverseFind(L'/');
			if (slash != -1) {
				CStringW slug = pageUrl.Mid(slash + 1);
				slug.Replace(L'-', L' ');
				album.title = slug;
			}
		}

		// Album cover candidates. Each <div class="albumImage"> block holds a full-
		// size <a href="..."> (a high-res scan, sharp as the now-playing art) and a
		// small <img src="...thumbs/..."> (only ~117 px). Prefer each block's full
		// image, with the thumbnails as fallbacks; the worker tries them in order.
		static const std::regex& reImgSrc = Re("<img[^>]*\\ssrc=\"(https?://[^\"]+\\.(?:jpg|jpeg|png|gif|webp))\"", true);
		static const std::regex& reImgHref = Re("href=\"(https?://[^\"]+\\.(?:jpg|jpeg|png|gif|webp))\"", true);
		std::vector<CStringW> thumbs, fulls;
		for (size_t imgPos = html.find("class=\"albumImage\""); imgPos != std::string::npos;
				imgPos = html.find("class=\"albumImage\"", imgPos + 1)) {
			const std::string imgBlock = html.substr(imgPos, 600);
			std::smatch m;
			if (std::regex_search(imgBlock, m, reImgSrc)) {
				thumbs.push_back(DecodeHtmlEntities(FromUtf8(m[1].str())));
			}
			if (std::regex_search(imgBlock, m, reImgHref)) {
				fulls.push_back(DecodeHtmlEntities(FromUtf8(m[1].str())));
			}
		}
		for (const auto& u : fulls)  { album.coverUrls.push_back(u); }
		for (const auto& u : thumbs) { album.coverUrls.push_back(u); }

		// album platforms: the "Platforms: <a>Saturn</a>, <a>Arcade</a><br>" line in
		// the page header (always "Platforms:" even for one). Distinct from the
		// sidebar "Platforms" nav (it has no colon).
		const size_t platPos = html.find("Platforms:");
		if (platPos != std::string::npos) {
			const size_t brPos = html.find("<br", platPos);
			const std::string platLine = html.substr(platPos, brPos == std::string::npos ? 600 : brPos - platPos);
			static const std::regex& rePlatName = Re(">([^<]+)</a>", false);
			for (auto it = std::sregex_iterator(platLine.begin(), platLine.end(), rePlatName); it != std::sregex_iterator(); ++it) {
				const CStringW n = Trimmed(DecodeHtmlEntities(FromUtf8((*it)[1].str())));
				if (!n.IsEmpty()) {
					if (!album.platforms.IsEmpty()) {
						album.platforms += L", ";
					}
					album.platforms += n;
				}
			}
		}

		return !album.tracks.empty();
	}

	CStringW ExtractTrackAudioUrl(const std::string& html, const CStringW& preferExt)
	{
		// Links may carry a query string and any letter case (".MP3?dl=1").
		static const std::regex& reAudio = Re("<audio[^>]*\\ssrc=\"([^\"]+)\"", true);
		static const std::regex& reMp3 = Re("href=\"(https?://[^\"]+\\.mp3(?:\\?[^\"]*)?)\"", true);
		static const std::regex& reOther = Re("href=\"(https?://[^\"]+\\.(?:flac|m4a|ogg)(?:\\?[^\"]*)?)\"", true);

		std::smatch match;
		if (!preferExt.IsEmpty()) {
			// testing aid (settings.json "preferAudioFormat"): that format's link first
			const CStringA ext(preferExt);
			std::string ext8(ext.GetString());
			const bool safe = !ext8.empty() && ext8.size() <= 5
				&& std::all_of(ext8.begin(), ext8.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
			if (safe) {
				const std::regex rePrefer("href=\"(https?://[^\"]+\\." + ext8 + "(?:\\?[^\"]*)?)\"", std::regex::ECMAScript | std::regex::icase);
				if (std::regex_search(html, match, rePrefer)) {
					return DecodeHtmlEntities(FromUtf8(match[1].str()));
				}
			}
		}
		if (std::regex_search(html, match, reAudio)
				|| std::regex_search(html, match, reMp3)
				|| std::regex_search(html, match, reOther)) {
			return DecodeHtmlEntities(FromUtf8(match[1].str()));
		}
		return CStringW();
	}

	AudioFormat AudioFormatOf(const CStringW& url)
	{
		CStringW path = url;
		const int q = path.FindOneOf(L"?#");
		if (q != -1) {
			path.Truncate(q);
		}
		path.MakeLower();
		const auto ends = [&](LPCWSTR ext) {
			const int n = static_cast<int>(wcslen(ext));
			return path.GetLength() > n && path.Right(n) == ext;
		};
		if (ends(L".mp3")) { return AudioFormat::Mp3; }
		if (ends(L".ogg") || ends(L".oga")) { return AudioFormat::Ogg; }
		if (ends(L".m4a") || ends(L".mp4") || ends(L".aac")) { return AudioFormat::M4a; }
		if (ends(L".flac")) { return AudioFormat::Flac; }
		return AudioFormat::Other;
	}

	CStringA BuildRandomAlbumForm(const std::vector<int>& types, const std::vector<int>& years, const std::vector<int>& platforms)
	{
		CStringA body;
		for (const auto v : types) {
			body.AppendFormat("type%%5B%%5D=%d&", v);
		}
		for (const auto v : years) {
			body.AppendFormat("year%%5B%%5D=%d&", v);
		}
		for (const auto v : platforms) {
			body.AppendFormat("category%%5B%%5D=%d&", v);
		}
		body += "randomAdvanced=Show+Me+A+Random+Album";
		return body;
	}

	namespace {
		// A letter or digit of a Latin-script word: the edge of a whole-word match.
		bool IsLatinWordChar(wchar_t c)
		{
			return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')
				|| (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) || c == L'\'';
		}

		bool IsDigitW(wchar_t c)
		{
			return (c >= L'0' && c <= L'9') || (c >= 0xFF10 && c <= 0xFF19);
		}

		// Kanji numerals used in episode numbers (第一話, 第十二話).
		bool IsKanjiDigit(wchar_t c)
		{
			static const wchar_t digits[] = L"一二三四五六七八九十百零〇";
			return wcschr(digits, c) != nullptr && c != 0;
		}

		// 'phrase' (lower case) as whole words somewhere in 'lower'.
		bool HasWords(const CStringW& lower, LPCWSTR phrase)
		{
			const int n = static_cast<int>(wcslen(phrase));
			for (int at = lower.Find(phrase); at >= 0; at = lower.Find(phrase, at + 1)) {
				const bool startOk = at == 0 || !IsLatinWordChar(lower[at - 1]);
				const bool endOk = at + n >= lower.GetLength() || !IsLatinWordChar(lower[at + n]);
				if (startOk && endOk) {
					return true;
				}
			}
			return false;
		}

		// ドラマ as its own word: not ドラマチック / ドラマティック / ドラマー.
		bool HasJapaneseDrama(const CStringW& s)
		{
			for (int at = s.Find(L"ドラマ"); at >= 0; at = s.Find(L"ドラマ", at + 1)) {
				const wchar_t next = at + 3 < s.GetLength() ? s[at + 3] : 0;
				if (next != L'チ' && next != L'テ' && next != L'ー') {
					return true;
				}
			}
			return false;
		}

		// 第N話 / 第N章 (N in digits or kanji numerals).
		bool HasNumbered(const CStringW& s, wchar_t unit)
		{
			for (int at = s.Find(L'第'); at >= 0; at = s.Find(L'第', at + 1)) {
				int i = at + 1;
				while (i < s.GetLength() && (IsDigitW(s[i]) || IsKanjiDigit(s[i]))) {
					i++;
				}
				if (i > at + 1 && i < s.GetLength() && s[i] == unit) {
					return true;
				}
			}
			return false;
		}

		// Lower-case Latin words of the title, in order.
		std::vector<CStringW> LatinWords(const CStringW& lower)
		{
			std::vector<CStringW> words;
			CStringW cur;
			for (int i = 0; i <= lower.GetLength(); i++) {
				const wchar_t c = i < lower.GetLength() ? lower[i] : 0;
				if (c && IsLatinWordChar(c) && c != L'\'') {
					cur += c;
				} else if (!cur.IsEmpty()) {
					words.push_back(cur);
					cur.Empty();
				}
			}
			return words;
		}

		// "Aerith Voice" / "Kairi's Voice": a short title naming whose voice it is.
		// Not "Voice of the Deep", "Hall of Voices", "Inner Voice", "Silent Voice".
		bool IsNamedVoiceTitle(const CStringW& lower)
		{
			const std::vector<CStringW> w = LatinWords(lower);
			if (w.size() < 2 || w.size() > 4 || w.back() != L"voice") {
				return false;
			}
			static const LPCWSTR notNames[] = {
				L"the", L"a", L"an", L"of", L"my", L"your", L"his", L"her", L"our", L"their", L"its",
				L"inner", L"silent", L"this", L"that", L"one", L"no", L"same", L"distant", L"lost",
				L"angel", L"angels", L"heavenly", L"sweet", L"gentle", L"crystal", L"wind", L"ocean",
				L"sea", L"forest", L"heart", L"soul", L"spirit", L"divine", L"sacred", L"secret",
				L"true", L"last", L"final", L"first", L"new", L"another", L"hidden", L"echoing",
			};
			const CStringW& before = w[w.size() - 2];
			for (const auto s : notNames) {
				if (before == s) {
					return false;
				}
			}
			return true;
		}
	}

	TitleCue SpokenTitleCue(const CStringW& title, CStringW* matched)
	{
		CStringW lower = title;
		lower.MakeLower();
		auto hit = [&](LPCWSTR what, TitleCue cue) {
			if (matched) {
				*matched = what;
			}
			return cue;
		};

		// Strong: the title says outright that the track is a drama or voice recording.
		static const LPCWSTR strongWords[] = {
			L"drama cd", L"drama disc", L"drama track", L"drama part", L"mini drama", L"audio drama",
			L"radio drama", L"voice drama", L"vocal drama", L"sound drama", L"cd drama",
			L"voice collection", L"voice clip", L"voice clips", L"voice sample", L"voice samples",
			L"cast comment", L"cast comments", L"cast talk", L"audio commentary", L"djcd", L"free talk",
		};
		for (const auto w : strongWords) {
			if (HasWords(lower, w)) {
				return hit(w, TitleCue::Strong);
			}
		}
		static const LPCWSTR strongJa[] = {
			L"ドラマcd", L"ドラマｃｄ", L"ミニドラマ", L"ボイスドラマ", L"ヴォイスドラマ", L"ボイスコレクション",
			L"ヴォイスコレクション", L"キャストコメント", L"ボイス集", L"ヴォイス集", L"セリフ集", L"台詞集",
		};
		for (const auto w : strongJa) {
			if (lower.Find(w) >= 0) {
				return hit(w, TitleCue::Strong);
			}
		}

		// Weak: common in drama and cutscene tracks, but also in some music titles
		// ("Message", "Talk", a "Prologue" cue), so the audio must agree.
		static const LPCWSTR weakWords[] = {
			L"drama", L"cutscene", L"cutscenes", L"cut scene", L"cut scenes", L"message", L"talk",
			L"narration", L"narrator", L"dialogue", L"dialog", L"monologue", L"skit", L"skits",
			L"commentary", L"interview", L"with voice",
		};
		for (const auto w : weakWords) {
			if (HasWords(lower, w)) {
				return hit(w, TitleCue::Weak);
			}
		}
		if (IsNamedVoiceTitle(lower)) {
			return hit(L"<name> voice", TitleCue::Weak);
		}
		// "Scene 12 ..." at the start, or "#scene": numbered cutscenes
		{
			CStringW t = lower;
			t.TrimLeft();
			if (t.Find(L"#scene") >= 0) {
				return hit(L"#scene", TitleCue::Weak);
			}
			if (t.Left(5) == L"scene") {
				int i = 5;
				while (i < t.GetLength() && (t[i] == L' ' || t[i] == L'.' || t[i] == L'_' || t[i] == L'-')) {
					i++;
				}
				if (i < t.GetLength() && IsDigitW(t[i])) {
					return hit(L"scene N", TitleCue::Weak);
				}
			}
		}
		if (HasJapaneseDrama(lower)) {
			return hit(L"ドラマ", TitleCue::Weak);
		}
		if (HasNumbered(lower, L'話')) {
			return hit(L"第N話", TitleCue::Weak);
		}
		if (HasNumbered(lower, L'章')) {
			return hit(L"第N章", TitleCue::Weak);
		}
		static const LPCWSTR weakJa[] = {
			L"ボイス", L"ヴォイス", L"セリフ", L"台詞", L"ナレーション", L"会話", L"メッセージ", L"トーク",
			L"コメント", L"朗読", L"インタビュー", L"おまけ",
		};
		for (const auto w : weakJa) {
			if (lower.Find(w) >= 0) {
				return hit(w, TitleCue::Weak);
			}
		}
		return TitleCue::None;
	}

	// An album title that says the whole album is spoken: a drama CD, a voice
	// collection, a radio show CD.
	bool IsSpokenTitle(const CStringW& title, CStringW* matched)
	{
		CStringW m;
		if (SpokenTitleCue(title, &m) == TitleCue::Strong) {
			if (matched) {
				*matched = m;
			}
			return true;
		}
		CStringW lower = title;
		lower.MakeLower();
		static const LPCWSTR albumWords[] = { L"drama", L"mini story", L"radio cd", L"djcd" };
		for (const auto w : albumWords) {
			if (HasWords(lower, w)) {
				if (matched) {
					*matched = w;
				}
				return true;
			}
		}
		if (HasJapaneseDrama(lower)) {
			if (matched) {
				*matched = L"ドラマ";
			}
			return true;
		}
		return false;
	}

	bool IsShortTrack(int durationSec, int minSec)
	{
		return durationSec > 0 && minSec > 0 && durationSec < minSec;
	}

	size_t Id3v2TagSize(const unsigned char* data, size_t len)
	{
		// "ID3" major minor flags size[4] (each size byte holds 7 bits)
		if (!data || len < 10 || data[0] != 'I' || data[1] != 'D' || data[2] != '3'
				|| data[3] == 0xFF || data[4] == 0xFF
				|| (data[6] | data[7] | data[8] | data[9]) & 0x80) {
			return 0;
		}
		const size_t body = (static_cast<size_t>(data[6]) << 21) | (static_cast<size_t>(data[7]) << 14)
			| (static_cast<size_t>(data[8]) << 7) | static_cast<size_t>(data[9]);
		const bool footer = (data[5] & 0x10) != 0;
		return 10 + body + (footer ? 10 : 0);
	}

	bool IsAllowedMediaUrl(const CStringW& url)
	{
		CStringW lower = url;
		lower.MakeLower();
		int start = 0;
		if (lower.Left(8) == L"https://") {
			start = 8;
		} else if (lower.Left(7) == L"http://") {
			start = 7;
		} else {
			return false;
		}
		int end = start;
		while (end < lower.GetLength() && lower[end] != L'/' && lower[end] != L'?' && lower[end] != L'#') {
			end++;
		}
		CStringW host = lower.Mid(start, end - start);
		if (host.Find(L'@') != -1 || host.Find(L'\\') != -1) {
			return false; // user info or odd separators: never ours
		}
		const int colon = host.Find(L':');
		if (colon != -1) {
			host.Truncate(colon);
		}
		static const LPCWSTR allowed[] = { L"khinsider.com", L"vgmtreasurechest.com" };
		for (const auto domain : allowed) {
			const int dl = static_cast<int>(wcslen(domain));
			if (host == domain) {
				return true;
			}
			if (host.GetLength() > dl + 1 && host.Right(dl + 1) == CStringW(L".") + domain) {
				return true;
			}
		}
		return false;
	}

	bool AlbumExclusiveTo(const CStringW& albumPlatforms, const std::vector<CStringW>& selected)
	{
		if (albumPlatforms.IsEmpty() || selected.empty()) {
			return true;
		}
		int pos = 0;
		while (pos < albumPlatforms.GetLength()) {
			const int comma = albumPlatforms.Find(L',', pos);
			CStringW tok = (comma < 0) ? albumPlatforms.Mid(pos) : albumPlatforms.Mid(pos, comma - pos);
			tok.Trim();
			if (!tok.IsEmpty()) {
				bool inSel = false;
				for (const auto& s : selected) {
					if (s.CompareNoCase(tok) == 0) {
						inSel = true;
						break;
					}
				}
				if (!inSel) {
					return false; // tagged for a console the user didn't pick
				}
			}
			if (comma < 0) {
				break;
			}
			pos = comma + 1;
		}
		return true;
	}
}
