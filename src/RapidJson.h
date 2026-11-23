// KH Radio - the rapidjson headers the app uses, with third-party warnings kept out of /W4.
// GPL-3.0-or-later (see COPYING.txt). rapidjson itself is MIT (thirdparty/rapidjson).
#pragma once

#pragma warning(push, 0)
#pragma warning(disable: 4127 4244 4267 4456 4457 4458 4702)
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#pragma warning(pop)
