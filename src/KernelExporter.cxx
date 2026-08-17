// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <max.kellermann@ionos.com>

#include "Frontend.hxx"
#include "Syntax.hxx"
#include "NumberParser.hxx"
#include "Pressure.hxx"
#include "CephDebugfs.hxx"
#include "system/Error.hxx"
#include "io/BufferedOutputStream.hxx"
#include "io/DirectoryReader.hxx"
#include "io/FileName.hxx"
#include "io/SmallTextFile.hxx"
#include "util/IterableSplitString.hxx"
#include "util/NumberParser.hxx"
#include "util/PrintException.hxx"
#include "util/StringCompare.hxx"
#include "util/StringSplit.hxx"

#include <cstdlib>

#include <fcntl.h>

using std::string_view_literals::operator""sv;

static std::string_view
ReadTextFile(FileDescriptor fd, std::span<char> buffer) noexcept
{
        ssize_t nbytes = fd.ReadAt(0, std::as_writable_bytes(buffer));
        if (nbytes < 0)
                return {};

        return {buffer.data(), static_cast<std::size_t>(nbytes)};
}

static std::string_view
ReadTextFile(FileAt f, std::span<char> buffer) noexcept
{
        UniqueFileDescriptor fd;
        if (!fd.Open(f, O_RDONLY|O_NOFOLLOW))
                return {};

        return ReadTextFile(fd, buffer);
}

static inline auto
ParseUserHz(std::string_view text)
{
	static const double user_hz_to_seconds = 1.0 / sysconf(_SC_CLK_TCK);
	return ParseUint64(text) * user_hz_to_seconds;
}

static void
ExportOopsWarnCounters(BufferedOutputStream &os)
{
	os.Write(R"(# HELP oops_count Number of kernel "oops"
# TYPE oops_count counter
# HELP warn_count Number of kernel warnings
# TYPE warn_count counter
# HELP softlockup_count Number of soft lockups
# TYPE softlockup_count counter
# HELP hardlockup_count Number of hard lockups
# TYPE hardlockup_count counter
# HELP rcu_stall_count Number of RCU stalls
# TYPE rcu_stall_count counter
)");

	UniqueFileDescriptor sys_kernel;
	if (!sys_kernel.Open("/sys/kernel", O_DIRECTORY|O_PATH))
		return;

	for (const char *name : {"oops_count", "warn_count", "softlockup_count", "hardlockup_count", "rcu_stall_count"}) {
		if (UniqueFileDescriptor f; f.OpenReadOnly({sys_kernel, name})) {
			WithSmallTextFile<64>(f, [&os, name](std::string_view contents){
				os.Fmt("{} {}\n", name, Strip(contents));
			});
		}
	}
}

static void
ExportHungTasks(BufferedOutputStream &os)
{
	os.Write(R"(# HELP hung_task_detect_count Total number of tasks that have been detected as hung since the system boot
# TYPE hung_task_detect_count counter
)");

	if (UniqueFileDescriptor f; f.OpenReadOnly("/proc/sys/kernel/hung_task_detect_count")) {
		WithSmallTextFile<64>(f, [&os](std::string_view contents){
			os.Fmt("hung_task_detect_count {}\n", Strip(contents));
		});
	}
}

static std::string_view
ReadLink(FileAt f, std::span<char> buffer) noexcept
{
	ssize_t result = readlinkat(f.directory.Get(), f.name, buffer.data(), buffer.size());
	if (result < 0)
		return {};

	return std::string_view{buffer.data(), static_cast<std::size_t>(result)};
}

static void
ExportOneHwmon(BufferedOutputStream &os, FileDescriptor hwmon_fd,
               std::string_view device, std::string_view chip_name,
               std::string_view prefix, unsigned start_index,
               double factor)
{
	for (unsigned i = start_index;; ++i) {
		char filename[64];
		char *end = fmt::format_to(filename, "{}{}"sv, prefix, i);
		strcpy(end, "_input");

		UniqueFileDescriptor fd;
		if (!fd.Open({hwmon_fd, filename}, O_RDONLY|O_NOFOLLOW))
			break;

		WithSmallTextFile<64>(fd, [&os, hwmon_fd,
                                           device, chip_name, prefix, factor,
                                           &filename, end](std::string_view contents){
			contents = StripRight(contents);
			unsigned value;
			if (!ParseIntegerTo(contents, value))
				return;

			os.Fmt("hwmon_{}{{device={:?},sensor={:?}"sv, prefix, device, std::string_view{filename, end});

                        if (!chip_name.empty())
                                os.Fmt(",chip_name={:?}"sv, StripRight(chip_name));

                        strcpy(end, "_label");
                        if (UniqueFileDescriptor label_fd; label_fd.Open({hwmon_fd, filename}, O_RDONLY|O_NOFOLLOW)) {
                                WithSmallTextFile<256>(label_fd, [&os](std::string_view label){
                                        os.Fmt(",label={:?}"sv, StripRight(label));
                                });
                        }

			os.Fmt("}} {:e}\n", value * factor);
		});
	}
}

static constexpr std::string_view
HwmonSymlinkToDeviceName(std::string_view s) noexcept
{
        SkipPrefix(s, "../../devices/"sv);

        while (true) {
                auto [a, b] = SplitLast(s, '/');
                if (!b.starts_with("hwmon"sv))
                        break;

                s = a;
        }

        return s;
}

static void
ExportHwmon(BufferedOutputStream &os)
{
	os.Write(R"(
# HELP hwmon_in Voltage [Volt]
# TYPE hwmon_in gauge
# HELP hwmon_fan Fan speed [rpm]
# TYPE hwmon_fan gauge
# HELP hwmon_temp Temperature [degrees Celsius]
# TYPE hwmon_temp gauge
# HELP hwmon_curr Current [Ampere]
# TYPE hwmon_curr gauge
# HELP hwmon_power Power [Watt]
# TYPE hwmon_power gauge
# HELP hwmon_energy Cumulative energy use [Joule]
# TYPE hwmon_energy counter
# HELP hwmon_humidity Humidity [%]
# TYPE hwmon_humidity gauge
)");

	UniqueFileDescriptor d;
	if (!d.Open("/sys/class/hwmon", O_DIRECTORY|O_RDONLY))
		return;

	DirectoryReader dr{std::move(d)};
	while (auto hwmon_name = dr.Read()) {
		if (IsSpecialFilename(hwmon_name))
			continue;

		UniqueFileDescriptor hwmon_fd;
		if (!hwmon_fd.Open({dr.GetFileDescriptor(), hwmon_name}, O_DIRECTORY|O_PATH))
			continue;

		char symlink_buffer[256];
		std::string_view device = HwmonSymlinkToDeviceName(ReadLink({dr.GetFileDescriptor(), hwmon_name}, symlink_buffer));

                char chip_name_buffer[256];
                const std::string_view chip_name = StripRight(ReadTextFile({hwmon_fd, "name"}, chip_name_buffer));

		ExportOneHwmon(os, hwmon_fd, device, chip_name, "in"sv, 0, 1e-3);
		ExportOneHwmon(os, hwmon_fd, device, chip_name, "fan"sv, 1, 1);
		ExportOneHwmon(os, hwmon_fd, device, chip_name, "temp"sv, 1, 1e-3);
		ExportOneHwmon(os, hwmon_fd, device, chip_name, "curr"sv, 1, 1e-3);
		ExportOneHwmon(os, hwmon_fd, device, chip_name, "power"sv, 1, 1e-6);
		ExportOneHwmon(os, hwmon_fd, device, chip_name, "energy"sv, 1, 1e-6);
		ExportOneHwmon(os, hwmon_fd, device, chip_name, "humidity"sv, 1, 1e-3);
	}
}

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

	os.Fmt(R"(loadavg{{period="1m"}} {:e}
loadavg{{period="5m"}} {:e}
loadavg{{period="15m"}} {:e}
)", load1, load5, load15);

	/* same output format as node_exporter */

	os.Fmt(R"(# HELP node_load1 1m load average.
# TYPE node_load1 gauge
node_load1 {:e}
# HELP node_load15 15m load average.
# TYPE node_load15 gauge
node_load15 {:e}
# HELP node_load5 5m load average.
# TYPE node_load5 gauge
node_load5 {:e}
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
		os.Fmt("meminfo{{name={:?}}} {}\n", name, nbytes);

		/* same output format as node_exporter */
		os.Fmt(R"(# HELP node_memory_{}_bytes Memory information field {}_bytes.
# TYPE node_memory_{}_bytes gauge
node_memory_{}_bytes {}
)",
		       name, name, name, name, nbytes);
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
# HELP node_forks_total Total number of forks.
# TYPE node_forks_total counter
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

				const double seconds = ParseUserHz(value);

				os.Fmt("node_cpu_seconds_total{{cpu={:?},mode={:?}}} {:e}\n",
				       name, mode, seconds);
			}
		} else if (name == "intr"sv) {
			auto value = Split(values, ' ').first;
			if (!value.empty())
				os.Fmt("node_intr_total {}\n", value);
		} else if (name == "ctxt"sv) {
			auto value = Split(values, ' ').first;
			if (!value.empty())
				os.Fmt("node_context_switches_total {}\n", value);
		} else if (name == "processes"sv) {
			auto value = Split(values, ' ').first;
			if (!value.empty())
				os.Fmt("node_forks_total {}\n", value);
		} else if (name == "procs_running"sv || name == "procs_blocked") {
			auto value = Split(values, ' ').first;
			if (!value.empty())
				os.Fmt("node_{} {}\n", name, value);
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
		os.Fmt("vmstat{{name={:?}}} {}\n", name, v);

		/* same output format as node_exporter */
		os.Fmt(R"(# HELP node_vmstat_{} /proc/vmstat information field {}.
# TYPE node_vmstat_{} untyped
node_vmstat_{} {}
)",
		       name, name, name, name, v);
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
				os.Fmt(R"(# HELP node_network_{}_total Network device statistic {}.
# TYPE node_network_{}_total counter
)",
				       name, name, name);

			os.Fmt("node_network_{}_total{{device={:?}}} {}\n", name, device, value);
		}

		first = false;
	}
}

static void
ExportProcNetSnmp(BufferedOutputStream &os, std::string_view s)
{
	while (true) {
		auto [label_line, rest1] = Split(s, '\n');
		auto [values_line, rest2] = Split(rest1, '\n');
		s = rest2;

		auto [protocol, labels] = Split(label_line, ':');
		auto [protocol2, values] = Split(values_line, ':');

		if (protocol.empty() || protocol != protocol2)
			break;

		labels = StripLeft(labels);
		values = StripLeft(values);

		while (true) {
			auto [label, more_labels] = Split(labels, ' ');
			auto [value, more_values] = Split(values, ' ');

			if (label.empty() || value.empty())
				break;

			labels = more_labels;
			values = more_values;

			os.Fmt(R"(
# HELP node_netstat_{}_{} Statistic {}{}.
# TYPE node_netstat_{}_{} untyped
node_netstat_{}_{} {}
)",
			       protocol, label, protocol, label,
			       protocol, label,
			       protocol, label, value);
		}
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
				os.Fmt(R"(# HELP node_disk_{} {}
# TYPE node_disk_{} {}
)",
					  c.name, c.help, c.name, c.type);

			os.Fmt("node_disk_{}{{device={:?}}} {:e}\n",
			       c.name, device,
			       value * c.factor);
		}

		first = false;
	}
}

template<std::size_t buffer_size>
static void
Export(BufferedOutputStream &os, auto &&file,
       std::invocable<BufferedOutputStream &, std::string_view> auto f)
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
		os.Fmt("# HELP {}\n# TYPE {} gauge\n{} {:e}\n",
		       some_help, some_name, some_name,
		       data.some.stall_time);

	if (full_name != nullptr && data.full.stall_time >= 0)
		os.Fmt("# HELP {}\n# TYPE {} gauge\n{} {:e}\n",
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

static auto
NextHex(std::string_view &line) noexcept
{
	auto [value, rest] = Split(StripLeft(line), ' ');
	line = rest;
	return ParseInteger<uint_least64_t>(value, 16);
}

static void
ExportIpVs(BufferedOutputStream &os)
{
	UniqueFileDescriptor f;
	if (!f.OpenReadOnly("/proc/net/ip_vs_stats"))
		return;

	os.Write(R"(
# HELP ip_vs_connections Number of IP_VS connections that were created
# TYPE ip_vs_connections counter
# HELP ip_vs_incoming_packets Number of incoming IP_VS packets
# TYPE ip_vs_incoming_packets counter
# HELP ip_vs_outgoing_packets Number of output IP_VS packets
# TYPE ip_vs_outgoing_packets counter
# HELP ip_vs_incoming_bytes Number of incoming IP_VS bytes
# TYPE ip_vs_incoming_bytes counter
# HELP ip_vs_outgoing_bytes Number of outgoing IP_VS bytes
# TYPE ip_vs_outgoing_bytes counter
)");

	WithSmallTextFile<1024>(f, [&os](std::string_view contents){
		 auto [header1, rest1] = Split(contents, '\n');
		auto [header2, rest2] = Split(rest1, '\n');
		auto [line, rest3] = Split(rest2, '\n');

		const auto total_conns = NextHex(line);
		const auto incoming_packets = NextHex(line);
		const auto outgoing_packets = NextHex(line);
		const auto incoming_bytes = NextHex(line);
		const auto outgoing_bytes = NextHex(line);

		if (total_conns && incoming_packets && outgoing_packets && incoming_bytes && outgoing_bytes) {
			os.Fmt(R"(
ip_vs_connections {}
ip_vs_incoming_packets {}
ip_vs_outgoing_packets {}
ip_vs_incoming_bytes {}
ip_vs_outgoing_bytes {}
)",
			       *total_conns,
			       *incoming_packets,
			       *outgoing_packets,
			       *incoming_bytes,
			       *outgoing_bytes);
		}
	});
}

static void
ExportKernel(BufferedOutputStream &os)
{
	ExportOopsWarnCounters(os);
	ExportHungTasks(os);
	ExportHwmon(os);
	Export<256>(os, "/proc/loadavg", ExportLoadAverage);
	Export<8192>(os, "/proc/meminfo", ExportMemInfo);
	Export<32768>(os, "/proc/stat", ExportStat);
	Export<16384>(os, "/proc/vmstat", ExportVmStat);
	Export<16384>(os, "/proc/net/dev", ExportProcNetDev);
	Export<8192>(os, "/proc/net/snmp", ExportProcNetSnmp);
	Export<8192>(os, "/proc/net/netstat", ExportProcNetSnmp);
	Export<16384>(os, "/proc/diskstats", ExportProcDiskstats);
	ExportPressure(os);
	ExportIpVs(os);
	ExportCeph(os);
}

int
main(int argc, char **argv) noexcept
try {
	if (argc > 1) {
		fmt::print(stderr, "Usage: {}\n", argv[0]);
		return EXIT_FAILURE;
	}

	return RunExporter(ExportKernel);
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
