// KH Radio - rapidjson helpers copied from MPC-BE's rapidjsonHelper.h. GPL-3.0-or-later.
#pragma once

#include <type_traits>
#include "rapidjson/document.h"
#include "TextUtil.h"

template <typename T>
inline bool getJsonValue(const rapidjson::Value& jsonValue, const char* name, T& value)
{
	if (const auto& it = jsonValue.FindMember(name); it != jsonValue.MemberEnd()) {
		if constexpr (std::is_same_v<T, int>) {
			if (it->value.IsInt()) {
				value = it->value.GetInt();
				return true;
			}
		} else if constexpr (std::is_same_v<T, double>) {
			if (it->value.IsNumber()) {
				value = it->value.GetDouble();
				return true;
			}
		} else if constexpr (std::is_same_v<T, bool>) {
			if (it->value.IsBool()) {
				value = it->value.GetBool();
				return true;
			}
		} else if constexpr (std::is_same_v<T, CStringW>) {
			if (it->value.IsString()) {
				value = UTF8ToWStr(it->value.GetString());
				return true;
			}
		}
	}

	return false;
}

inline const rapidjson::Value* GetJsonArray(const rapidjson::Value& jsonValue, const char* name)
{
	const auto& jsonArray = jsonValue.FindMember(name);
	if (jsonArray != jsonValue.MemberEnd() && jsonArray->value.IsArray() && !jsonArray->value.Empty()) {
		return &jsonArray->value;
	}

	return nullptr;
}

inline const rapidjson::Value* GetJsonObject(const rapidjson::Value& jsonValue, const char* name)
{
	const auto& jsonObject = jsonValue.FindMember(name);
	if (jsonObject != jsonValue.MemberEnd() && jsonObject->value.IsObject()) {
		return &jsonObject->value;
	}

	return nullptr;
}
