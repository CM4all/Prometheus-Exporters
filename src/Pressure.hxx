// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "io/SmallTextFile.hxx"

#include <string_view>

struct PressureItemValues {
	double avg10 = -1, avg60 = -1, avg300 = -1;
	double stall_time = -1;
};

struct PressureValues {
	PressureItemValues some, full;
};

void
ParsePressureLine(PressureValues &p, std::string_view line);

auto
ReadPressureFile(auto &&file)
{
	PressureValues result;

	ForEachTextLine<1024>(file, [&result](std::string_view line){
		ParsePressureLine(result, line);
	});

	return result;
}
