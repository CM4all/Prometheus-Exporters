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
#include "NumberParser.hxx"
#include "Pressure.hxx"
#include "system/Error.hxx"
#include "io/BufferedOutputStream.hxx"
#include "io/SmallTextFile.hxx"
#include "util/IterableSplitString.hxx"
#include "util/PrintException.hxx"
#include "util/StringCompare.hxx"
#include "util/StringSplit.hxx"

#include <cstdlib>

#include <fcntl.h>
#include <inttypes.h>

using std::string_view_literals::operator""sv;

static void
ExportLoadAverage(BufferedOutputStream &os, std::string_view s)
{
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
ExportMemInfo(BufferedOutputStream &os, std::string_view s)
{
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
ExportVmStat(BufferedOutputStream &os, std::string_view s)
{
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

template<std::size_t buffer_size>
static void
Export(BufferedOutputStream &os, auto &&file,
       Invocable<BufferedOutputStream &, std::string_view> auto f)
{
	WithSmallTextFile<buffer_size>(file, [&](std::string_view contents){
		f(os, contents);
	});
}

static void
ExportPressure(BufferedOutputStream &os, auto &&file,
	       const char *some_name, const char *some_help,
	       const char *full_name, const char *full_help)
try {
	auto data = ReadPressureFile(file);

	if (some_name != nullptr && data.some.stall_time >= 0)
		os.Format("# HELP %s\n# TYPE %s gauge\n%s %e\n",
			  some_name, some_help, some_name,
			  data.some.stall_time);

	if (full_name != nullptr && data.full.stall_time >= 0)
		os.Format("# HELP %s\n# TYPE %s gauge\n%s %e\n",
			  full_name, full_help, full_name,
			  data.full.stall_time);
} catch (const std::system_error &e) {
	if (!IsFileNotFound(e))
		throw;
}

static void
ExportPressure(BufferedOutputStream &os)
{
	ExportPressure(os, "/proc/pressure/cpu",
		       "node_pressure_cpu_waiting_seconds_total",
		       "Total time in seconds that processes have waited for CPU time",
		       nullptr, nullptr);

	ExportPressure(os, "/proc/pressure/io",
		       "node_pressure_io_waiting_seconds_total",
		       "Total time in seconds that processes have waited due to IO congestion",
		       "node_pressure_io_stalled_seconds_total",
		       "Total time in seconds no process could make progress due to IO congestion");

	ExportPressure(os, "/proc/pressure/memory",
		       "node_pressure_memory_waiting_seconds_total",
		       "Total time in seconds that processes have waited for memory",
		       "node_pressure_memory_stalled_seconds_total",
		       "Total time in seconds no process could make progress due to memory congestion");
}

static void
ExportKernel(BufferedOutputStream &os)
{
	Export<256>(os, "/proc/loadavg", ExportLoadAverage);
	Export<8192>(os, "/proc/meminfo", ExportMemInfo);
	Export<16384>(os, "/proc/vmstat", ExportVmStat);
	ExportPressure(os);
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
