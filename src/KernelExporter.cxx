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

#include "Frontend.hxx"
#include "CgroupConfig.hxx"
#include "TextFile.hxx"
#include "NumberParser.hxx"
#include "io/BufferedOutputStream.hxx"
#include "io/DirectoryReader.hxx"
#include "io/Open.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "util/IterableSplitString.hxx"
#include "util/PrintException.hxx"
#include "util/StringCompare.hxx"
#include "util/StringSplit.hxx"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>

#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/stat.h>

using std::string_view_literals::operator""sv;

static void
ExportLoadAverage(BufferedOutputStream &os)
{
	char buffer[267];
	auto s = ReadTextFile(FileDescriptor{AT_FDCWD}, "/proc/loadavg",
			      buffer, sizeof(buffer));

	os.Write(R"(# HELP loadavg Load average.
# TYPE loadavg gauge
)");

	auto [load1s, rest1] = Split(s, ' ');
	const double load1 = ParseDouble(load1s);

	auto [load5s, rest5] = Split(rest1, ' ');
	const double load5 = ParseDouble(load5s);

	auto [load15s, rest15] = Split(rest5, ' ');
	const double load15 = ParseDouble(load15s);

	os.Format(R"(loadavg{period="1m"} %e
loadavg{period="5m"} %e
loadavg{period="15m"} %e
)", load1, load5, load15);
}

static void
ExportMemInfo(BufferedOutputStream &os)
{
	char buffer[8192];
	auto s = ReadTextFile(FileDescriptor{AT_FDCWD}, "/proc/meminfo",
			      buffer, sizeof(buffer));

	os.Write(R"(# HELP meminfo Kernel memory info
# TYPE meminfo gauge
)");

	for (const auto line : IterableSplitString(s, '\n')) {
		auto [name, value] = Split(line, ':');
		if (name.empty())
			continue;

		value = Strip(value);
		if (value.empty())
			continue;

		uint64_t unit = 1;
		if (RemoveSuffix(value, " kB"sv))
			unit = 1024;

		os.Format("meminfo{name=\"%.*s\"} %" PRIu64 "\n",
			  int(name.size()), name.data(),
			  ParseUint64(value) * unit);
	}
}

static void
ExportVmStat(BufferedOutputStream &os)
{
	char buffer[16384];
	auto s = ReadTextFile(FileDescriptor{AT_FDCWD}, "/proc/vmstat",
			      buffer, sizeof(buffer));

	os.Write(R"(# HELP vmstat
# TYPE vmstat untyped
)");

	for (const auto line : IterableSplitString(s, '\n')) {
		auto [name, value] = Split(line, ' ');
		if (!name.empty() && !value.empty())
			os.Format("vmstat{name=\"%.*s\"} %" PRIu64 "\n",
				  int(name.size()), name.data(),
				  ParseUint64(value));
	}
}

static void
ExportKernel(BufferedOutputStream &os)
{
	ExportLoadAverage(os);
	ExportMemInfo(os);
	ExportVmStat(os);
}

int
main(int argc, char **argv) noexcept
try {
	if (argc > 1) {
		fprintf(stderr, "Usage: %s\n", argv[0]);
		return EXIT_FAILURE;
	}

	return RunExporter(ExportKernel);
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
