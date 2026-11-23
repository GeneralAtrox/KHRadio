# Licence

KH Radio is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. The full text is in `COPYING.txt`.

Parts of the source (`src/KHInsider.*`, `src/KHWebSession.*`, `src/KHRadioDlg.*`,
`src/KHRadioLayout.h`, and helpers in `src/TextUtil.h` and `src/JsonHelper.h`) are
derived from MPC-BE (https://sourceforge.net/projects/mpcbe/) and its KH Radio
fork, which are GPL-3.0-or-later; their original notices are kept in the files.

Third-party components keep their own licences:

- `thirdparty/rapidjson` - MIT (see `thirdparty/rapidjson/license.txt`).
- `thirdparty/minimp3` - CC0 / public domain (see the header of `minimp3.h`).
- `thirdparty/curl-impersonate` - libcurl (curl licence) and zlib (zlib licence)
  runtime DLLs, see `thirdparty/curl-impersonate/README.md`.
- The WebView2 SDK is downloaded at build time from NuGet under Microsoft's licence.
