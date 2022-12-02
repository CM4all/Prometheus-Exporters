/*
 * Copyright 2012-2020 CM4all GmbH
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

#include "Pressure.hxx"
#include "NumberParser.hxx"

using std::string_view_literals::operator""sv;

static auto
ParsePressureLine(std::string_view line)
{
	static constexpr double micro_factor = 1e-6;

	PressureItemValues result;

	for (std::string_view i : IterableSplitString(line, ' ')) {
		const auto [name, value] = Split(i, '=');
		if (value.data() == nullptr)
			continue;

		if (name == "avg10"sv)
			result.avg10 = ParseDouble(value);
		else if (name == "avg60"sv)
			result.avg60 = ParseDouble(value);
		else if (name == "avg300"sv)
			result.avg300 = ParseDouble(value);
		else if (name == "total"sv)
			result.stall_time = ParseUint64(value) * micro_factor;
	}

	return result;
}

void
ParsePressureLine(PressureValues &p, std::string_view line)
{
	line = Strip(line);

	const auto [s, rest] = Split(line, ' ');
	if (s == "some"sv)
		p.some = ParsePressureLine(rest);
	else if (s == "full"sv)
		p.full = ParsePressureLine(rest);
}
