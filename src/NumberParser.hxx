/*
 * Copyright 2020-2022 CM4all GmbH
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "util/CharUtil.hxx"
#include "util/StringSplit.hxx"
#include "util/StringStrip.hxx"

#include <cmath>
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
