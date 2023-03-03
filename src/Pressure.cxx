// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

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
