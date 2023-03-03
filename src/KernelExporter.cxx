// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "Frontend.hxx"
#include "Syntax.hxx"
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

	/* obsolete (proprietary) output format */

	os.Format(R"(loadavg{period="1m"} %e
loadavg{period="5m"} %e
loadavg{period="15m"} %e
)", load1, load5, load15);

	/* same output format as node_exporter */

	os.Format(R"(# HELP node_load1 1m load average.
# TYPE node_load1 gauge
node_load1 %e
# HELP node_load15 15m load average.
# TYPE node_load15 gauge
node_load15 %e
# HELP node_load5 5m load average.
# TYPE node_load5 gauge
node_load5 %e
)", load1, load5, load15);
}

static void
ExportMemInfo(BufferedOutputStream &os, std::string_view s)
{
	/* obsolete (proprietary) output format */
	os.Write(R"(# HELP meminfo Kernel memory info
# TYPE meminfo gauge
)");

	for (const auto line : IterableSplitString(s, '\n')) {
		auto [_name, value] = Split(line, ':');
		if (_name.empty())
			continue;

		const auto name = SanitizeMetricName(_name);

		value = Strip(value);
		if (value.empty())
			continue;

		uint64_t unit = 1;
		if (RemoveSuffix(value, " kB"sv))
			unit = 1024;

		const uint64_t nbytes = ParseUint64(value) * unit;

		/* obsolete (proprietary) output format */
		os.Format("meminfo{name=\"%.*s\"} %" PRIu64 "\n",
			  int(name.size()), name.data(),
			  nbytes);

		/* same output format as node_exporter */
		os.Format(R"(# HELP node_memory_%.*s_bytes Memory information field %.*s_bytes.
# TYPE node_memory_%.*s_bytes gauge
)"
			  "node_memory_%.*s_bytes %" PRIu64 "\n",
			  int(name.size()), name.data(),
			  int(name.size()), name.data(),
			  int(name.size()), name.data(),
			  int(name.size()), name.data(),
			  nbytes);
	}
}

/**
 * @see https://www.kernel.org/doc/html/latest/filesystems/proc.html#miscellaneous-kernel-statistics-in-proc-stat
 */
static void
ExportStat(BufferedOutputStream &os, std::string_view s)
{
	os.Write(R"(
# HELP node_cpu_seconds_total Seconds the CPUs spent in each mode.
# TYPE node_cpu_seconds_total counter
# HELP node_intr_total Total number of interrupts serviced.
# TYPE node_intr_total counter
# HELP node_context_switches_total Total number of context switches.
# TYPE node_context_switches_total counter
# HELP node_procs_blocked Number of processes blocked waiting for I/O to complete.
# TYPE node_procs_blocked gauge
# HELP node_procs_running Number of processes in runnable state.
# TYPE node_procs_running gauge
)");

	for (const auto line : IterableSplitString(s, '\n')) {
		auto [name, values] = Split(line, ' ');
		if (name.empty() || values.empty())
			continue;

		if (SkipPrefix(name, "cpu"sv)) {
			if (name.empty())
				continue;

			static constexpr std::array cpu_columns = {
				"user", "nice", "system", "idle", "iowait",
				"irq", "softirq",
				"steal",
				"guest", "guest_nice",
			};

			for (const char *mode : cpu_columns) {
				auto [value, rest] = Split(values, ' ');
				if (value.empty())
					break;

				values = rest;

				os.Format("node_cpu_seconds_total{cpu=\"%.*s\",mode=\"%s\"} %.*s\n",
					  int(name.size()), name.data(),
					  mode,
					  int(value.size()), value.data());
			}
		} else if (name == "intr"sv) {
			auto value = Split(values, ' ').first;
			if (!value.empty())
				os.Format("node_intr_total %.*s\n", int(value.size()), value.data());
		} else if (name == "ctxt"sv) {
			auto value = Split(values, ' ').first;
			if (!value.empty())
				os.Format("node_context_switches_total %.*s\n", int(value.size()), value.data());
		} else if (name == "procs_running"sv || name == "procs_blocked") {
			auto value = Split(values, ' ').first;
			if (!value.empty())
				os.Format("node_%.*s %.*s\n",
					  int(name.size()), name.data(),
					  int(value.size()), value.data());
		}
	}
}

static void
ExportVmStat(BufferedOutputStream &os, std::string_view s)
{
	/* obsolete (proprietary) output format */
	os.Write(R"(# HELP vmstat
# TYPE vmstat untyped
)");

	for (const auto line : IterableSplitString(s, '\n')) {
		auto [name, value] = Split(line, ' ');
		if (name.empty() || value.empty())
			continue;

		const uint64_t v = ParseUint64(value);

		/* obsolete (proprietary) output format */
		os.Format("vmstat{name=\"%.*s\"} %" PRIu64 "\n",
			  int(name.size()), name.data(),
			  v);

		/* same output format as node_exporter */
		os.Format(R"(# HELP node_vmstat_%.*s /proc/vmstat information field %.*s.
# TYPE node_vmstat_%.*s untyped
)"
			  "node_vmstat_%.*s %" PRIu64 "\n",
			  int(name.size()), name.data(),
			  int(name.size()), name.data(),
			  int(name.size()), name.data(),
			  int(name.size()), name.data(),
			  v);
	}
}

static void
ExportProcNetDev(BufferedOutputStream &os, std::string_view s)
{
	static constexpr const char *proc_net_dev_columns[] = {
		"receive_bytes",
		"receive_packets",
		"receive_errors",
		"receive_dropped",
		"receive_fifo",
		"receive_frame",
		"receive_compressed",
		"receive_multicast",
		"transmit_bytes",
		"transmit_packets",
		"transmit_errors",
		"transmit_dropped",
		"transmit_fifo",
		"transmit_colls",
		"transmit_carrier",
		"transmit_compressed",
		nullptr
	};

	bool first = true;
	for (const auto line : IterableSplitString(s, '\n')) {
		auto [device, values] = Split(line, ':');
		if (device.empty() || values.empty())
			continue;

		device = StripLeft(device);

		for (const char *const* c = proc_net_dev_columns;
		     *c != nullptr; ++c) {
			const char *name = *c;

			auto [value_s, rest] = Split(StripLeft(values), ' ');
			if (value_s.empty())
				break;

			values = StripLeft(rest);

			const uint64_t value = ParseUint64(value_s);

			if (first)
				os.Format(R"(# HELP node_network_%s_total Network device statistic %s.
# TYPE node_network_%s_total counter
)",
					  name, name, name);

			os.Format("node_network_%s_total{device=\"%.*s\"} %" PRIu64 "\n",
				  name, int(device.size()), device.data(),
				  value);
		}

		first = false;
	}
}

[[gnu::pure]]
static bool
IgnoreDisk(std::string_view device) noexcept
{
	return device.starts_with("ram"sv) || device.starts_with("loop"sv);
}

static void
ExportProcDiskstats(BufferedOutputStream &os, std::string_view s)
{
	static constexpr struct {
		const char *name;
		const char *help;
		const char *type;
		double factor = 1;
	} proc_diskstats_columns[] = {
		{
			"reads_completed_total",
			"The total number of reads completed successfully.",
			"counter",
		},
		{
			"reads_merged_total",
			"The total number of reads merged.",
			"counter",
		},
		{
			"read_bytes_total",
			"The total number of bytes read successfully.",
			"counter",
			512,
		},
		{
			"read_time_seconds_total",
			"The total number of seconds spent by all reads",
			"counter",
			0.001,
		},

		{
			"writes_completed_total",
			"The total number of writes completed successfully.",
			"counter",
		},
		{
			"writes_merged_total",
			"The total number of writes merged.",
			"counter",
		},
		{
			"write_bytes_total",
			"The total number of bytes write successfully.",
			"counter",
			512,
		},
		{
			"write_time_seconds_total",
			"The total number of seconds spent by all writes",
			"counter",
			0.001,
		},

		{
			"io_now",
			"The number of I/Os currently in progress.",
			"gauge",
		},

		{
			"io_time_seconds_total",
			"Total seconds spent doing I/Os.",
			"counter",
			0.001,
		},

		{
			"io_time_weighted_seconds_total",
			"The weighted # of seconds spent doing I/Os.",
			"counter",
			0.001,
		},

		{
			"discards_completed_total",
			"The total number of discards completed successfully.",
			"counter",
		},
		{
			"discards_merged_total",
			"The total number of discards merged.",
			"counter",
		},

		// TODO implement the rest
	};

	bool first = true;
	for (auto line : IterableSplitString(s, '\n')) {
		// skip "major"
		line = Split(StripLeft(line), ' ').second;

		// skip "minor"
		line = Split(StripLeft(line), ' ').second;

		auto [device, values] = Split(StripLeft(line), ' ');
		if (IgnoreDisk(device))
			continue;

		for (const auto &c : proc_diskstats_columns) {
			auto [value_s, rest] = Split(StripLeft(values), ' ');
			if (value_s.empty())
				break;

			values = StripLeft(rest);

			const uint64_t value = ParseUint64(value_s);

			if (first)
				os.Format(R"(# HELP node_disk_%s %s
# TYPE node_disk_%s %s
)",
					  c.name, c.help, c.name, c.type);

			os.Format("node_disk_%s{device=\"%.*s\"} %e\n",
				  c.name, int(device.size()), device.data(),
				  value * c.factor);
		}

		first = false;
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
			  some_help, some_name, some_name,
			  data.some.stall_time);

	if (full_name != nullptr && data.full.stall_time >= 0)
		os.Format("# HELP %s\n# TYPE %s gauge\n%s %e\n",
			  full_help, full_name, full_name,
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
	Export<32768>(os, "/proc/stat", ExportStat);
	Export<16384>(os, "/proc/vmstat", ExportVmStat);
	Export<16384>(os, "/proc/net/dev", ExportProcNetDev);
	Export<16384>(os, "/proc/diskstats", ExportProcDiskstats);
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
