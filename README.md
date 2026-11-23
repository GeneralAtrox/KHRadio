# KH Radio

A small standalone Windows app that plays continuous video-game-music radio from
[KHInsider](https://downloads.khinsider.com): it rolls random albums using the site's
"Random Album Advanced" filters (type, year, platform), keeps the next album queued
ahead, shows the album cover, and remembers what you have heard.

Version 1.2.0. It was extracted from the KH Radio panel of the MPC-BE fork in this
repository and shares nothing with MPC-BE: its own exe, window class, settings, data
folders and browser profile. It registers no file types, shell extensions or autoplay
handlers and ignores command-line arguments.

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
- Track audio is looked up a few tracks ahead of the one playing, so a new album
  starts after one page load instead of one per track.

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
| Queue and position to resume | `%APPDATA%\KHRadio\queue.json` |
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
and track page parser on small hand-written pages in `tests\fixtures`, response
classification, retry delays, the panel layout at 100/150/200% DPI, history files
(round trip, damaged file, damaged backup, empty list) and the browser session's
shutdown. `--live` also checks the saved sign-in against the real site, and
`--login` opens the sign-in window first.

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
