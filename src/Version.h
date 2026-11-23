// KH Radio - the one place the version number is set (used by the .rc, the
// project's manifest identity and the log). GPL-3.0-or-later (see COPYING.txt).
#pragma once

#define KHRADIO_VERSION_MAJOR 1
#define KHRADIO_VERSION_MINOR 4
#define KHRADIO_VERSION_PATCH 0
#define KHRADIO_VERSION_STR   "1.4.0"
#define KHRADIO_VERSION_STR4  "1.4.0.0"


#ifndef RC_INVOKED
namespace KHVersion
{
	// Checks at compile time that the strings above match the numbers.
	constexpr bool Matches(const char* s, int major, int minor, int patch, int build)
	{
		const int want[4] = { major, minor, patch, build };
		for (int part = 0; part < 4; part++) {
			if (*s < '0' || *s > '9') {
				return build < 0 && part == 3 && *s == '\0';
			}
			int n = 0;
			while (*s >= '0' && *s <= '9') {
				n = n * 10 + (*s++ - '0');
			}
			if (n != want[part]) {
				return false;
			}
			if (*s == '.') {
				s++;
			} else {
				return *s == '\0' && (part == 3 || (part == 2 && build < 0));
			}
		}
		return *s == '\0';
	}
	static_assert(Matches(KHRADIO_VERSION_STR, KHRADIO_VERSION_MAJOR, KHRADIO_VERSION_MINOR, KHRADIO_VERSION_PATCH, -1),
		"KHRADIO_VERSION_STR does not match the version numbers");
	static_assert(Matches(KHRADIO_VERSION_STR4, KHRADIO_VERSION_MAJOR, KHRADIO_VERSION_MINOR, KHRADIO_VERSION_PATCH, 0),
		"KHRADIO_VERSION_STR4 does not match the version numbers");
}
#endif
