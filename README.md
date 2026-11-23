# KH Radio

A small standalone Windows app that plays continuous video-game-music radio from
[KHInsider](https://downloads.khinsider.com): it rolls random albums using the site's
"Random Album Advanced" filters (type, year, platform), keeps the next album queued
ahead, shows the album cover, and remembers what you have heard.

It was extracted from the KH Radio panel of the MPC-BE fork in this repository and
shares nothing with MPC-BE: its own exe, window class, settings, data folders and
browser profile. It registers no file types, shell extensions or autoplay handlers
and ignores command-line arguments.

## Using it

- **Sign in to KHInsider...** opens a small browser window; sign in there once. The
  sign-in stays in KH Radio's own WebView2 profile.
- Pick filters (Ctrl-click to select several), then **Show Me A Random Album**. With
  nothing to resume, a random album starts by itself once signed in.
- The right side shows the cover, track, album and platforms, a seek bar, previous /
  play-pause / next, volume, and the upcoming queue (double-click to jump).
- Media keys (play/pause, next, previous, stop) work while KH Radio runs.
- Double-click an album in **History** to play it again.
- Tracks that will not play (for example an `.ogg` with no decoder installed) are
  logged and skipped.

## Where data lives

| What | Where |
|------|-------|
| Settings (filters, volume, window position) | `%APPDATA%\KHRadio\settings.json` |
| Listening history | `%APPDATA%\KHRadio\khradio_history.json` |
| Queue and position to resume | `%APPDATA%\KHRadio\queue.json` |
| Album covers in use | `%APPDATA%\KHRadio\covers\` |
| Debug log | `%APPDATA%\KHRadio\khradio_debug.log` |
| Browser profile (KHInsider sign-in) | `%LOCALAPPDATA%\KHRadio\WebView2` |

On first run, if these are empty, KH Radio **copies** (never moves) the MPC-BE fork's
history (`%APPDATA%\MPC-BE\khradio_history.json`), its `KHRadio*` filter settings
(read from `HKCU\Software\MPC-BE\Settings`) and its browser profile
(`%LOCALAPPDATA%\MPC-BE\KHInsider.WebView2`, skipped if that browser is running).

## Requirements

- Windows 10 or 11, x64.
- Microsoft Edge WebView2 Runtime (included with Windows 11).
- `libcurl-impersonate.dll` and `zlib.dll` next to the exe are optional: they help
  cover downloads get past Cloudflare; without them WinHTTP is used.

## Building

Visual Studio 2022 (v143) with the "C++ MFC for latest build tools" component.
Open `KHRadio.sln` and build `Release|x64`, or:

```
msbuild KHRadio.sln /p:Configuration=Release /p:Platform=x64
```

The first build downloads the WebView2 SDK from NuGet (checksum-verified) into
`_build\WebView2`. Output goes to `_build\Release\KHRadio.exe` with the curl DLLs
copied beside it. MFC and the C runtime are linked statically.

## Layout

- `src\` - the app. `KHInsider.*`, `KHWebSession.*`, `KHRadioDlg.*` and
  `KHRadioLayout.h` come from the fork (adapted); the rest is new.
- `thirdparty\rapidjson` (MIT), `thirdparty\minimp3` (CC0),
  `thirdparty\curl-impersonate` (runtime DLLs, see its README),
  `thirdparty\webview2` (SDK restore script).

## Licence

GPL-3.0-or-later, as the MPC-BE code it is derived from. See `COPYING.txt` and
`LICENSE.md`.
