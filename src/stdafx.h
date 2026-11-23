// KH Radio - common header. GPL-3.0-or-later (see COPYING.txt).
#pragma once

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#define _AFX_ALL_WARNINGS
#include <afxwin.h>
#include <afxext.h>
#include <afxcmn.h>
#include <afxdisp.h>
#include <atlstr.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "resource.h"
