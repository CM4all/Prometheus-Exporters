// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "Frontend.hxx"
#include "CgroupConfig.hxx"
#include "Pressure.hxx"
#include "NumberParser.hxx"
#include "io/BufferedOutputStream.hxx"
#include "io/DirectoryReader.hxx"
#include "io/FileAt.hxx"
#include "io/Open.hxx"
#include "io/SmallTextFile.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "util/IterableSplitString.hxx"
#include "util/PrintException.hxx"
#include "util/StringCompare.hxx"

#include <algorithm>
#include <concepts>
#include <cstdlib>
#include <map>
#include <string>

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

using std::string_view_literals::operator""sv;

static inline auto
ParseUserHz(std::string_view text)
{
	static const double user_hz_to_seconds = 1.0 / sysconf(_SC_CLK_TCK);
	return ParseUint64(text) * user_hz_to_seconds;
}

static inline auto
ParseUsec(std::string_view text)
{
	static constexpr double usec_to_seconds = 0.000001;
	return ParseUint64(text) * usec_to_seconds;
}

static int64_t
ReadUint64File(auto &&file)
{
	return WithSmallTextFile<64>(file, ParseUint64);
}

[[gnu::pure]]
static double
ReadDoubleFile(FileAt file, double factor=1.0)
{
	return WithSmallTextFile<64>(file, ParseUint64) * factor;
}

static void
ForEachNameValue(FileAt file,
		 std::invocable<std::string_view, std::string_view> auto f)
{
	ForEachTextLine<4096>(file, [&f](std::string_view line){
		line = Strip(line);
		auto [a, b] = Split(line, ' ');
		if (!a.empty() && b.data() != nullptr)
			f(a, b);
	});
}

struct CgroupCpuacctValues {
	double usage = -1, user = -1, system = -1;
};

struct CgroupMemoryValues {
	int64_t usage = -1, kmem_usage = -1, memsw_usage = -1;
	int64_t swap_usage = -1;
	int64_t failcnt = -1, kmem_failfnt = -1, memsw_failcnt = -1;
	std::map<std::string, uint64_t> stat;
};

struct CgroupPidsValues {
	int64_t current = -1;
};

struct CgroupValues {
	CgroupCpuacctValues cpuacct;
	CgroupMemoryValues memory;
	CgroupPidsValues pids;
	PressureValues cpu_pressure, io_pressure, memory_pressure;
};

struct CgroupsData {
	std::map<std::string, CgroupValues> groups;
};

struct WalkContext {
	const CgroupExporterConfig &config;
	CgroupsData &data;

	char path[4096];
	size_t length = 0;

	WalkContext(const CgroupExporterConfig &_config,
		    CgroupsData &_data) noexcept
		:config(_config), data(_data)
	{
		path[0] = 0;
	}

	void Dive(FileAt file);
	void HandleRegularFile(FileAt file);
	void DoWalk(UniqueFileDescriptor directory_fd);
};

enum class WalkDecision { IGNORE, DIRECTORY, REGULAR };

static WalkDecision
CheckWalkFile(FileAt file)
{
	if (*file.name == '.')
		return WalkDecision::IGNORE;

	struct stat st;
	if (fstatat(file.directory.Get(), file.name, &st,
		    AT_NO_AUTOMOUNT|AT_SYMLINK_NOFOLLOW) < 0)
		return WalkDecision::IGNORE;

	if (S_ISREG(st.st_mode))
		return WalkDecision::REGULAR;
	else if (S_ISDIR(st.st_mode))
		return WalkDecision::DIRECTORY;
	else
		return WalkDecision::IGNORE;
}

inline void
WalkContext::Dive(FileAt file)
{
	const size_t old_length = length;
	const size_t name_length = strlen(file.name);

	if (old_length + name_length + 2 >= sizeof(path))
		return;

	if (length > 0)
		path[length++] = '/';
	memcpy(path + length, file.name, name_length);
	length += name_length;
	path[length] = 0;

	DoWalk(OpenDirectory(file.directory, file.name, O_NOFOLLOW));

	length = old_length;
	path[length] = 0;
}

static char *
Substitute(char *dest, const char *src, const char *a, const char *b) noexcept
{
	while (true) {
		const char *t = strstr(src, a);
		if (t == nullptr)
			break;

		dest = std::copy(src, t, dest);
		dest = stpcpy(dest, b);
		src = t + strlen(a);
	}

	return stpcpy(dest, src);
}

inline void
WalkContext::HandleRegularFile(FileAt file)
{
	const char *base = file.name;
	const char *group_name = path;

	char unescape_buffer[sizeof(path)];
	if (strstr(group_name, "\\x2d") != nullptr) {
		/* unescape the dash; it was escaped by systemd, but
		   backslashes in file names are terrible to use */
		Substitute(unescape_buffer, group_name, "\\x2d", "-");
		group_name = unescape_buffer;
	}

	auto &group = data.groups[group_name];

	static constexpr double nano_factor = 1e-9;

	if (StringIsEqual(base, "cpuacct.usage")) {
		// cgroup1
		group.cpuacct.usage = ReadDoubleFile(file, nano_factor);
	} else if (StringIsEqual(base, "cpuacct.stat")) {
		// cgroup1
		ForEachNameValue(file, [&group](auto name, auto value){
			if (name == "user"sv)
				group.cpuacct.user = ParseUserHz(value);
			else if (name == "system"sv)
				group.cpuacct.system = ParseUserHz(value);
		});
	} else if (StringIsEqual(base, "cpu.stat")) {
		// cgroup2
		ForEachNameValue(file, [&group](auto name, auto value){
			if (name == "usage_usec"sv)
				group.cpuacct.usage = ParseUsec(value);
			else if (name == "user_usec"sv)
				group.cpuacct.user = ParseUsec(value);
			else if (name == "system_usec"sv)
				group.cpuacct.system = ParseUsec(value);
		});
	} else if (/* cgroup1 */
		   StringIsEqual(base, "memory.usage_in_bytes") ||
		   /* cgroup2 */
		   StringIsEqual(base, "memory.current")) {
		group.memory.usage = ReadUint64File(file);
	} else if (StringIsEqual(base, "memory.swap.current")) {
		// cgroup2
		group.memory.swap_usage = ReadUint64File(file);
	} else if (StringIsEqual(base, "memory.kmem.usage_in_bytes")) {
		// cgroup1
		group.memory.kmem_usage = ReadUint64File(file);
	} else if (StringIsEqual(base, "memory.memsw.usage_in_bytes")) {
		// cgroup1
		group.memory.memsw_usage = ReadUint64File(file);
	} else if (StringIsEqual(base, "memory.stat")) {
		ForEachNameValue(file, [&group](auto name, auto value){
			if (name.ends_with("_limit"sv))
				/* skip hierarchical_memory_limit */
				return;

			group.memory.stat[std::string{name}] = ParseUint64(value);
		});
	} else if (StringIsEqual(base, "pids.current")) {
		group.pids.current = ReadUint64File(file);
	} else if (StringIsEqual(base, "cpu.pressure")) {
		group.cpu_pressure = ReadPressureFile(file);
	} else if (StringIsEqual(base, "io.pressure")) {
		group.io_pressure = ReadPressureFile(file);
	} else if (StringIsEqual(base, "memory.pressure")) {
		group.memory_pressure = ReadPressureFile(file);
	}
}

void
WalkContext::DoWalk(UniqueFileDescriptor directory_fd)
{
	DirectoryReader r(std::move(directory_fd));

	const bool opaque = config.opaque_paths.find(path) != config.opaque_paths.end();

	while (auto name = r.Read()) {
		const FileAt file{r.GetFileDescriptor(), name};

		switch (CheckWalkFile(file)) {
		case WalkDecision::IGNORE:
			break;

		case WalkDecision::DIRECTORY:
			if (!opaque && !config.CheckIgnoreName(name))
				Dive(file);
			break;

		case WalkDecision::REGULAR:
			try {
				HandleRegularFile(file);
			} catch (...) {
				PrintException(std::current_exception());
			}
			break;
		}
	}
}

static auto
CollectCgroup1(const CgroupExporterConfig &config)
{
	CgroupsData data;

	for (const char *mnt : {"/sys/fs/cgroup/cpuacct", "/sys/fs/cgroup/memory", "/sys/fs/cgroup/pids", "/sys/fs/cgroup/unified"}) {
		WalkContext ctx(config, data);

		try {
			ctx.DoWalk(OpenDirectory(FileDescriptor(AT_FDCWD), mnt));
		} catch (...) {
			PrintException(std::current_exception());
		}
	}

	return data;
}

static auto
CollectCgroup2(const CgroupExporterConfig &config)
{
	CgroupsData data;

	try {
		WalkContext ctx(config, data);
		ctx.DoWalk(OpenDirectory(FileDescriptor(AT_FDCWD), "/sys/fs/cgroup"));
	} catch (...) {
		PrintException(std::current_exception());
	}

	return data;
}

[[gnu::pure]]
static bool
HasCgroup2() noexcept
{
	struct stat st;
	return stat("/sys/fs/cgroup/cgroup.subtree_control", &st) == 0;
}

static auto
CollectCgroup(const CgroupExporterConfig &config)
{
	return HasCgroup2()
		? CollectCgroup2(config)
		: CollectCgroup1(config);
}

static void
WriteCpuacct(BufferedOutputStream &os, std::string_view group, std::string_view type,
	     double value)
{
	if (value >= 0)
		os.Fmt("cgroup_cpu_usage{{groupname={:?},type={:?}}} {:e}\n",
		       group, type, value);
}

static void
WriteMemory(BufferedOutputStream &os, std::string_view group, std::string_view type,
	    int64_t value)
{
	if (value >= 0)
		os.Fmt("cgroup_memory_usage{{groupname={:?},type={:?}}} {}\n",
		       group, type, value);
}

static void
WriteMemory(BufferedOutputStream &os, std::string_view group, std::string_view type,
	    uint64_t value)
{
	os.Fmt("cgroup_memory_usage{{groupname={:?},type={:?}}} {}\n",
	       group, type, value);
}

static void
WritePids(BufferedOutputStream &os, std::string_view group, const CgroupPidsValues &pids)
{
	if (pids.current >= 0)
		os.Fmt("cgroup_pids{{groupname={:?}}} {}\n",
		       group, pids.current);
}

static void
WritePressureRatio(BufferedOutputStream &os, std::string_view group,
		   std::string_view resource, std::string_view type,
		   std::string_view window, double value)
{
	if (value >= 0)
		os.Fmt("cgroup_pressure_ratio{{groupname={:?},resource={:?},type={:?},window={:?}}} {:e}\n",
		       group, resource, type, window, value);
}

static void
WritePressureRatio(BufferedOutputStream &os, std::string_view group,
		   std::string_view resource, std::string_view type,
		   const PressureItemValues &values)
{
	WritePressureRatio(os, group, resource, type, "10"sv, values.avg10);
	WritePressureRatio(os, group, resource, type, "60"sv, values.avg60);
	WritePressureRatio(os, group, resource, type, "300"sv, values.avg300);
}

static void
WritePressureRatio(BufferedOutputStream &os, std::string_view group,
		   std::string_view resource,
		   const PressureValues &values)
{
	WritePressureRatio(os, group, resource, "some"sv, values.some);
	WritePressureRatio(os, group, resource, "full"sv, values.full);
}

static void
WritePressureStallTime(BufferedOutputStream &os, std::string_view group,
		       std::string_view resource, std::string_view type,
		       double value)
{
	if (value >= 0)
		os.Fmt("cgroup_pressure_stall_time{{groupname={:?},resource={:?},type={:?}}} {:e}\n",
		       group, resource, type, value);
}

static void
WritePressureStallTime(BufferedOutputStream &os, std::string_view group,
		       std::string_view resource, std::string_view type,
		       const PressureItemValues &values)
{
	WritePressureStallTime(os, group, resource, type, values.stall_time);
}

static void
WritePressureStallTime(BufferedOutputStream &os, std::string_view group,
		       std::string_view resource,
		       const PressureValues &values)
{
	WritePressureStallTime(os, group, resource, "some"sv, values.some);
	WritePressureStallTime(os, group, resource, "full"sv, values.full);
}

static void
DumpCgroup(BufferedOutputStream &os, const CgroupsData &data)
{
	os.Write(R"(# HELP cgroup_cpu_usage CPU usage in seconds
# TYPE cgroup_cpu_usage counter
)");

	for (const auto &i : data.groups) {
		const std::string_view group = i.first;
		const auto &cpu = i.second.cpuacct;
		WriteCpuacct(os, group, "user"sv, cpu.user);
		WriteCpuacct(os, group, "system"sv, cpu.system);
		WriteCpuacct(os, group, "total"sv, cpu.usage);
	}

	os.Write(R"(# HELP cgroup_memory_usage Memory usage in bytes
# TYPE cgroup_memory_usage gauge
)");

	for (const auto &i : data.groups) {
		const std::string_view group = i.first;
		const auto &memory = i.second.memory;
		WriteMemory(os, group, "total"sv, memory.usage);
		WriteMemory(os, group, "swap"sv, memory.swap_usage);
		WriteMemory(os, group, "kmem.total"sv, memory.kmem_usage);
		WriteMemory(os, group, "memsw.total"sv, memory.memsw_usage);

		for (const auto &m : memory.stat)
			WriteMemory(os, group, m.first, m.second);
	}

	os.Write(R"(# HELP cgroup_memory_failures Memory limit failures
# TYPE cgroup_memory_failures counter
)");

	for (const auto &i : data.groups) {
		const std::string_view group = i.first;
		const auto &memory = i.second.memory;
		WriteMemory(os, group, "memory"sv, memory.failcnt);
		WriteMemory(os, group, "kmem"sv, memory.kmem_failfnt);
		WriteMemory(os, group, "memsw"sv, memory.memsw_failcnt);
	}

	os.Write(R"(# HELP cgroup_pids Process/Thread count
# TYPE cgroup_pids gauge
)");

	for (const auto &i : data.groups) {
		const std::string_view group = i.first;
		const auto &pids = i.second.pids;
		WritePids(os, group, pids);
	}

	os.Write(R"(# HELP cgroup_pressure_ratio Pressure stall ratio
# TYPE cgroup_pressure_ratio gauge
)");

	for (const auto &i : data.groups) {
		const std::string_view group = i.first;
		WritePressureRatio(os, group, "cpu"sv, i.second.cpu_pressure);
		WritePressureRatio(os, group, "io"sv, i.second.io_pressure);
		WritePressureRatio(os, group, "memory"sv, i.second.memory_pressure);
	}

	os.Write(R"(# HELP cgroup_pressure_stall_time Pressure stall time
# TYPE cgroup_pressure_stall_time counter
)");

	for (const auto &i : data.groups) {
		const std::string_view group = i.first;
		WritePressureStallTime(os, group, "cpu"sv, i.second.cpu_pressure);
		WritePressureStallTime(os, group, "io"sv, i.second.io_pressure);
		WritePressureStallTime(os, group, "memory"sv, i.second.memory_pressure);
	}
}

static void
ExportCgroup(const CgroupExporterConfig &config, BufferedOutputStream &os)
{
	DumpCgroup(os, CollectCgroup(config));
}

int
main(int argc, char **argv) noexcept
try {
	const char *config_file = "/etc/cm4all/prometheus-exporters/cgroup.yml";
	if (argc >= 2)
		config_file = argv[1];

	if (argc > 2) {
		fmt::print(stderr, "Usage: {} CONFIGFILE\n", argv[0]);
		return EXIT_FAILURE;
	}

	const auto config = LoadCgroupExporterConfig(config_file);

	return RunExporter([&](BufferedOutputStream &os){
		ExportCgroup(config, os);
	});
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
