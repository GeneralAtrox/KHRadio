# KH Radio

A small standalone Windows app that plays continuous video-game-music radio from
[KHInsider](https://downloads.khinsider.com): it rolls random albums using the site's
"Random Album Advanced" filters (type, year, platform), keeps the next album queued
ahead, shows the album cover, and remembers what you have heard.

Version 1.3.0. It started as the KH Radio panel of an MPC-BE fork and now lives in its
own repository. It shares nothing with MPC-BE: its own exe, window class, settings,
data folders and browser profile. It registers no file types, URL schemes, shell
extensions or autoplay handlers and ignores command-line arguments.

## Using it

- **Sign in to KHInsider...** opens a small browser window; sign in there once. The
  sign-in stays in KH Radio's own WebView2 profile.
- Pick filters (Ctrl-click to select several), then **Show Me A Random Album**. With
  nothing to resume, a random album starts by itself once signed in.
- The right side shows the cover, track, album and platforms, a seek bar, previous /
  play-pause / next, volume, and the upcoming queue (double-click to jump).
- KH Radio appears in the Windows media controls (the volume flyout and the lock
  screen) with the track, album and cover, so media keys and headset buttons work
  from any app. The keyboard's media keys also work while KH Radio has focus.
- Double-click an album in **History** to play it again.
- **Share a song**: the up-arrow button next to the transport copies the playing track
  to the clipboard, ready to paste into any chat. Right-click a queue row for "Share
  this track" or "Share this album", or a history row for "Share this album".
- **Play a shared song**: copy the message someone sent you, then press the down-arrow
  button (or Ctrl+V anywhere in KH Radio). See [Sharing](#sharing).
- Track audio is looked up a few tracks ahead of the one playing, so a new album
  starts after one page load instead of one per track.

## Skipping drama and cutscene tracks

Many game soundtracks, Japanese ones most of all, carry tracks that are people
talking: drama-CD episodes, cutscene dialogue, voice-actor messages, narration.
With **Filter spoken / short / silent tracks** ticked, KH Radio passes over them.

- **Album titles** that say the whole album is spoken ("Drama CD", "Voice
  Collection", "DJCD", "Mini Story", ドラマCD, ミニドラマ...) are rerolled in random mode.
- **Track titles** are matched as whole words, never inside other words: "Drama CD",
  "Mini Drama", "Cast Comment", "Free Talk", ボイスドラマ, セリフ集 are enough on their own.
  Looser cues ("Drama", "Message", "Talk", "Cutscene", "Scene 3", "#scene", 第2話,
  メッセージ, トーク, セリフ, ナレーション...) only count when the audio agrees.
  "Dramatic", ドラマチック, "Hall of Voices" or "Skitchin'" never match.
- **The audio** is checked a few tracks ahead of playback: two short windows (about
  256 KB each) are fetched, one after the tag and one from the middle, and decoded.
  Music holds steady pitches and repeats its beat; talking glides in pitch, pauses
  between phrases and has no beat. A track counts as talking only when **both**
  windows sound like it, so a spoken intro followed by music is kept, and so are
  chiptune, piano, music box, ambient pieces and medleys.
- **Long tracks** (8 MB or more) that pass that check are also sampled at 16 points
  along their length (about 128 KB each). A talk show that alternates talking with
  music can have music at both of the first two windows; if a quarter or more of
  the 16 points sound like talking, the track is skipped, in any album.
- **The album decides how sure it has to be.** In an album that has sounded musical,
  only very clear talking is skipped (one or two odd tracks are kept unless they are
  plainly speech); in an album that is mostly talking, less is needed; a title cue
  plus talking-like audio is enough anywhere.
- **Silent** tracks (both windows silent) and **short** ones are skipped too. Short
  means under 10 seconds; change it with `"minTrackSeconds"` in `settings.json`
  (0 turns it off). A track whose length is unknown is never short.

A skipped track stays in the queue as a dimmed row that says why ("sounds like
talking (drama or cutscene)", "too short", "silent", "title says drama / voice
track"). Next, previous and the media keys step over it. Right-click it and choose
**Play anyway** to hear it: it plays at once, and the filter leaves that track alone
from then on (remembered in `%APPDATA%\KHRadio\keep_tracks.json`). The debug log
has one line per checked track with every measure and the rule that decided.

**How well it works.** The rule was set on half of a labelled sample of
game-soundtrack tracks and checked on the other half. The sample deliberately
included music the 1.3 filter had wrongly skipped; on both halves the new rule
dropped none of the music, those included. It found about four in five talking tracks overall (most drama-CD and
radio-show tracks, every drama track titled as one) and about three in five drama or
cutscene tracks inside otherwise musical soundtracks. What it still misses: talking
over music or sound effects (cutscenes mixed with score), short exchanges inside
an album that has sounded musical, unless the title gives them away, and tracks under
8 MB that mix talking with music. The long-track rule was set on one 25-minute radio-show
track and five other tracks from the same album, not on the labelled sample. It was tuned to
keep music first, so it errs on the side of playing talking rather than skipping a song.

## Sharing

What is copied is plain text: the track and album on the first line and the track's
own KHInsider page on the second, for example

```
Opening Theme — Example Game Soundtrack
https://downloads.khinsider.com/game-soundtracks/album/<album>/<track>
```

The link opens in a browser too, so it works for someone without KH Radio. Nothing
else is shared: no audio file addresses and nothing about your account.

When you play a shared link, KH Radio looks for the first KHInsider album or track link
anywhere in the clipboard text (surrounding words, `<brackets>`, trailing punctuation,
markdown links, `http`/`www.`, query strings and percent-encoding are all fine). Only
`downloads.khinsider.com/game-soundtracks/album/...` links are accepted; anything else
is refused before any request is made, and the status line says "No KH Radio link on
the clipboard". A track link loads its album and starts at that track (if the track is
no longer on the album page, the album plays from the start and the status line says
so); an album link plays like a history replay. Either way the radio carries on
afterwards. If you are not signed in, the link waits and plays once you are. Rolling
an album, double-clicking history or the queue, or skipping drops a link still loading.

## When something goes wrong

- If KHInsider can't be reached, is checking the browser, or is limiting requests,
  the status line says so and when it will try again. KH Radio re-checks by itself
  (10 s, growing to 5 min), and at once when the network comes back or the PC wakes
  from sleep. Tracks already queued keep playing; the radio carries on by itself
  once the site answers.
- If the sign-in has expired, the status line asks you to sign in again; the radio
  resumes after that.
- If KHInsider changes its page format, the status line says so: KH Radio may then
  need an update.
- A damaged settings, history or queue file is never overwritten: it is renamed
  with `.unreadable-<date>` or `.corrupt-<date>` added, and the history is restored
  from its backup (`khradio_history.json.bak`) where possible.

## Audio formats

KH Radio takes a track's MP3 when the track page offers one, otherwise its M4A,
FLAC or OGG (the first link the page offers among those).

- **MP3, M4A (AAC / ALAC), FLAC** stream from KHInsider through Windows' own decoders.
  If an M4A will not stream, KH Radio downloads that one file and plays it from the
  audio cache instead.
- **OGG (Ogg Vorbis)** is decoded by KH Radio itself (stb_vorbis), so no Store
  extension is needed: a few tracks ahead of the one playing, the file is downloaded
  (up to 64 MB) and decoded to a WAV in the audio cache. Jumping straight to an OGG
  track that is not ready yet shows "Preparing ..." and it starts as soon as it is.
  An `.ogg` that is not Vorbis (Opus, say) is handed to Windows to stream; it is
  skipped, with a log line, only if that fails too.
- The audio cache (`%LOCALAPPDATA%\KHRadio\cache\`) holds only the playing track and
  the next few, at most 500 MB in all (the oldest go first). A track's files are
  deleted once it is no longer playing or coming up, and the folder is emptied when
  KH Radio starts and exits. Nothing outside that folder is touched.
- To try a format on purpose (for example to check OGG playback on an album that
  also has MP3s), set `"preferAudioFormat": "ogg"` (or `"m4a"`, `"flac"`, `"mp3"`) in
  `settings.json` with KH Radio closed: that format is then taken first wherever a
  track offers it. Remove the line to go back to MP3 first.

## Where data lives

| What | Where |
|------|-------|
| Settings (filters, volume, window position) | `%APPDATA%\KHRadio\settings.json` |
| Listening history (and one backup) | `%APPDATA%\KHRadio\khradio_history.json`, `.bak` |
| Queue and position to resume (skipped rows too) | `%APPDATA%\KHRadio\queue.json` |
| Tracks the filter leaves alone ("Play anyway") | `%APPDATA%\KHRadio\keep_tracks.json` |
| Debug log (rotated at 4 MB to `.old`) | `%APPDATA%\KHRadio\khradio_debug.log` |
| Album covers | `%LOCALAPPDATA%\KHRadio\covers\` |
| Audio cache (OGG tracks as WAV, fallback M4A downloads; 500 MB at most, emptied at start and exit) | `%LOCALAPPDATA%\KHRadio\cache\` |
| Browser profile (KHInsider sign-in) | `%LOCALAPPDATA%\KHRadio\WebView2` |
| Saved pages for bug reports (off by default) | `%LOCALAPPDATA%\KHRadio\debug\` |

Covers moved from `%APPDATA%\KHRadio\covers` in 1.0 to `%LOCALAPPDATA%` in 1.1; they
are moved over on first start.

To help diagnose a parsing problem, set `"saveDebugPages": true` in `settings.json`
(with KH Radio closed): the last album page and track page fetched are then kept in
the `debug` folder above.

On a real first run (no `settings.json`), KH Radio **copies** (never moves) the MPC-BE
fork's history (`%APPDATA%\MPC-BE\khradio_history.json`), its `KHRadio*` filter
settings (read from `HKCU\Software\MPC-BE\Settings`) and its browser profile
(`%LOCALAPPDATA%\MPC-BE\KHInsider.WebView2`, skipped if that browser is running).
This happens once; `settings.json` records that it was done.

## Requirements

- Windows 10 or 11, x64.
- Microsoft Edge WebView2 Runtime (included with Windows 11).
- Optional: `libcurl-impersonate.dll` and `zlib.dll` next to the exe help cover
  downloads get past Cloudflare; without them WinHTTP is used.

## Building

Visual Studio 2022 (v143) with the "C++ MFC for latest build tools" component.
Open `KHRadio.sln` and build `Release|x64`, or from PowerShell:

```
.\build.ps1 -Configuration Release -Tests
```

`build.ps1` finds Visual Studio with vswhere, restores the WebView2 SDK from NuGet
(checksum-verified) into `_build\WebView2`, builds, optionally builds and runs the
tests, and prints a short verdict (full logs in `_build\logs`). Output goes to
`_build\Release\KHRadio.exe` with the curl DLLs copied beside it. MFC and the C
runtime are linked statically. The code builds at warning level 4 with SDL checks
and `/permissive-`.

To build on another machine over ssh and bring the exe back to `bin\`:

```
.\tools\build-remote.ps1 -HostName <ssh host> -RemoteDir <folder on that machine> -Configurations Release,Debug -Smoke
```

The version number is set in one place, `src\Version.h`; the resource file, the
manifest identity and the log line all read it.

## Tests

`tests\KHRadioTests` is a console program. By default it runs offline: the album
and track page parser on small hand-written pages in `tests\fixtures`, share links
(building them, finding them in messy pasted text, refusing lookalike hosts), response
classification, retry delays, the panel layout at 100/150/200% DPI, history files
(round trip, damaged file, damaged backup, empty list), the talking-track filter
(generated audio: tones, a chord with a beat, a music box, a chiptune arpeggio, an
ambient pad, noise bursts, 4 Hz modulated noise, synthetic speech, silence, a quiet
intro before loud music; title cues with invented titles; the album context; the
short-track threshold; skipped rows in the queue and queue.json; the "Play anyway"
list) and the browser session's shutdown. `--synth` prints every measure of the
generated audio. `--live` also checks the saved sign-in against the real site, and
`--login` opens the sign-in window first.

`--eval <list.tsv> <cache folder> <out.csv>` runs the talking-track detector on real
tracks (one per line: id, label, half, ..., audio address in the 11th column, album
and track title in the 12th and 13th), fetching two windows per track at a polite
pace and keeping them in the cache folder, and writes every measure to the CSV.
`tools\build-remote.ps1 -EvalList/-EvalCache/-EvalOut` runs it on the build machine.
No list ships with KH Radio.

## Layout

- `src\` - the app. `KHInsider.*`, `KHParse.*`, `KHWebSession.*`, `KHRadioDlg.*`,
  `History.*` and `KHRadioLayout.h` come from the fork (adapted); the rest is new.
- `tests\` - the test program and its hand-written fixtures.
- `tools\` - the remote build script.
- `thirdparty\rapidjson` (MIT), `thirdparty\minimp3` (CC0),
  `thirdparty\curl-impersonate` (runtime DLLs, see its README),
  `thirdparty\webview2` (SDK restore script).

## Licence

GPL-3.0-or-later, as the MPC-BE code it is derived from. See `COPYING.txt` and
`LICENSE.md`.
