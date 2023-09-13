// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "util/CharUtil.hxx"
#include "util/StringSplit.hxx"
#include "util/StringStrip.hxx"

#include <cmath>
#include <cstdint>
#include <string_view>
#include <utility>

template<typename T>
[[gnu::pure]]
T
ParseUnsignedT(std::string_view text) noexcept
{
	text = StripLeft(text);

	T value = 0;
	for (char ch : text) {
		if (!IsDigitASCII(ch))
			break;

		value = value * T(10) + T(ch - '0');
	}

	return value;
}

[[gnu::pure]]
inline auto
ParseUnsigned(std::string_view text) noexcept
{
	return ParseUnsignedT<unsigned>(text);
}

[[gnu::pure]]
inline auto
ParseUnsignedLong(std::string_view text) noexcept
{
	return ParseUnsignedT<unsigned long>(text);
}

[[gnu::pure]]
inline auto
ParseUint64(std::string_view text) noexcept
{
	return ParseUnsignedT<uint64_t>(text);
}

[[gnu::pure]]
inline double
ParseDouble(std::string_view text) noexcept
{
	const auto [a, b] = Split(text, '.');

	return (double)ParseUint64(a) +
		(double)ParseUint64(b) * pow(10, -(double)b.size());
}
