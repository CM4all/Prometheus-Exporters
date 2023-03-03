// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "Frontend.hxx"
#include "ProcessConfig.hxx"
#include "ProcessInfo.hxx"
#include "ProcessIterator.hxx"
#include "NumberParser.hxx"
#include "io/BufferedOutputStream.hxx"
#include "io/DirectoryReader.hxx"
#include "io/FileAt.hxx"
#include "io/SmallTextFile.hxx"
#include "io/Open.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "system/Error.hxx"
#include "util/IterableSplitString.hxx"
#include "util/PrintException.hxx"
#include "util/StringCompare.hxx"
#include "util/StringStrip.hxx"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>

using std::string_view_literals::operator""sv;

template<std::size_t buffer_size>
std::string
ReadTextFile(auto &&file)
{
	return WithSmallTextFile<buffer_size>(file, [](std::string_view contents){
		return std::string{contents};
	});
}

struct ProcessStatus {
	unsigned voluntary_ctxt_switches = 0;
	unsigned nonvoluntary_ctxt_switches = 0;
};

static auto
ParseProcessStatus(std::string_view text)
{
	ProcessStatus result;

	for (auto line : IterableSplitString(text, '\n')) {
		line = Strip(line);
		if (line.empty())
			continue;

		switch (line.front()) {
		case 'n':
			if (SkipPrefix(line, "nonvoluntary_ctxt_switches:"sv))
				result.nonvoluntary_ctxt_switches = ParseUnsigned(line);
			break;

		case 'v':
			if (SkipPrefix(line, "voluntary_ctxt_switches:"sv))
				result.voluntary_ctxt_switches = ParseUnsigned(line);
			break;
		}
	}

	return result;
}

struct ProcessStat {
	std::string_view comm;
	char state = 0;
	unsigned long minflt = 0, majflt = 0;
	unsigned long utime = 0, stime = 0;
	unsigned long vsize = 0, rss = 0;
};

static auto
ParseProcessStat(std::string_view text)
{
	ProcessStat result;

	std::string_view s;

	std::tie(s, text) = Split(text, ' '); // pid

	std::tie(s, text) = Split(text, ' '); // comm
	if (!s.empty() && s.front() == '(' && s.back() == ')') {
		s = s.substr(1, s.size() - 2);
	}

	result.comm = s;

	std::tie(s, text) = Split(text, ' '); // state
	if (!s.empty())
		result.state = s.front();

	std::tie(s, text) = Split(text, ' '); // ppid
	std::tie(s, text) = Split(text, ' '); // pgrp
	std::tie(s, text) = Split(text, ' '); // session
	std::tie(s, text) = Split(text, ' '); // tty_nr
	std::tie(s, text) = Split(text, ' '); // tpgid
	std::tie(s, text) = Split(text, ' '); // flags

	std::tie(s, text) = Split(text, ' '); // minflt
	result.minflt = ParseUnsignedLong(s);

	std::tie(s, text) = Split(text, ' '); // cminflt

	std::tie(s, text) = Split(text, ' '); // majflt
	result.majflt = ParseUnsignedLong(s);

	std::tie(s, text) = Split(text, ' '); // cmajflt

	std::tie(s, text) = Split(text, ' '); // utime
	result.utime = ParseUnsignedLong(s);

	std::tie(s, text) = Split(text, ' '); // stime
	result.stime = ParseUnsignedLong(s);

	std::tie(s, text) = Split(text, ' '); // cutime
	std::tie(s, text) = Split(text, ' '); // cstime
	std::tie(s, text) = Split(text, ' '); // priority
	std::tie(s, text) = Split(text, ' '); // nice
	std::tie(s, text) = Split(text, ' '); // num_threads
	std::tie(s, text) = Split(text, ' '); // itrealvalue
	std::tie(s, text) = Split(text, ' '); // starttime

	std::tie(s, text) = Split(text, ' '); // vsize
	result.vsize = ParseUnsignedLong(s);

	std::tie(s, text) = Split(text, ' '); // rss
	result.rss = ParseUnsignedLong(s);

	return result;
}

struct ProcessGroupData {
	unsigned n_procs = 0, n_threads = 0;
	unsigned voluntary_ctxt_switches = 0, nonvoluntary_ctxt_switches = 0;
	unsigned long minflt = 0, majflt = 0;
	unsigned long utime = 0, stime = 0;
	unsigned long vsize = 0, rss = 0;

	auto &operator+=(const ProcessStatus &src) noexcept {
		voluntary_ctxt_switches += src.voluntary_ctxt_switches;
		nonvoluntary_ctxt_switches += src.nonvoluntary_ctxt_switches;
		return *this;
	}

	auto &operator+=(const ProcessStat &src) noexcept {
		minflt += src.minflt;
		majflt += src.majflt;
		utime += src.utime;
		stime += src.stime;
		vsize += src.vsize;
		rss += src.rss;
		return *this;
	}
};

using ProcessGroupMap = std::unordered_map<std::string, ProcessGroupData>;

static void
CollectProcess(ProcessGroupData &group, unsigned, FileDescriptor pid_fd)
{
	group += WithSmallTextFile<4096>(FileAt{pid_fd, "status"},
					 ParseProcessStatus);
	group += WithSmallTextFile<1024>(FileAt{pid_fd, "stat"},
					 ParseProcessStat);
}

static auto
CollectProcessGroups(const ProcessExporterConfig &config, FileDescriptor proc_fd)
{
	ProcessGroupMap groups;

	ForEachProcess(proc_fd, [&](unsigned, FileDescriptor pid_fd){
		char exe[4096];
		ssize_t rl = readlinkat(pid_fd.Get(), "exe", exe, sizeof(exe));
		if (rl < 0 || size_t(rl) >= sizeof(exe))
			return;

		std::string_view name(exe, rl);
		RemoveSuffix(name, " (deleted)"sv);

		auto slash = SplitLast(name, '/');
		if (slash.second.data() != nullptr)
			name = slash.second;

		if (name.empty())
			return;

		const auto stat = WithSmallTextFile<1024>(FileAt{pid_fd, "stat"},
							  ParseProcessStat);

		ProcessInfo info;
		info.comm = std::string{stat.comm};
		info.exe = std::string{name};
		info.cmdline = ReadTextFile<4096>(FileAt{pid_fd, "cmdline"});
		std::replace(info.cmdline.begin(), info.cmdline.end(),
			     '\0', ' ');

		auto group_name = config.MakeName(info);
		if (group_name.empty())
			return;

		auto e = groups.emplace(std::move(group_name),
					ProcessGroupData{});
		auto &group = e.first->second;
		++group.n_procs;

		ForEachProcessThread(pid_fd, [&](unsigned tid, FileDescriptor tid_fd){
			++group.n_threads;
			CollectProcess(group, tid, tid_fd);
		});
	});

	return groups;
}

static void
DumpProcessGroups(BufferedOutputStream &os, const ProcessGroupMap &groups)
{
	os.Write(R"(# HELP namedprocess_namegroup_context_switches_total Context switches
# TYPE namedprocess_namegroup_context_switches_total counter
)");

	for (const auto &i : groups)
		os.Format("namedprocess_namegroup_context_switches_total{groupname=\"%s\",ctxswitchtype=\"nonvoluntary\"} %u\n"
			  "namedprocess_namegroup_context_switches_total{groupname=\"%s\",ctxswitchtype=\"voluntary\"} %u\n",
			  i.first.c_str(), i.second.nonvoluntary_ctxt_switches,
			  i.first.c_str(), i.second.voluntary_ctxt_switches);

	os.Write(R"(# HELP namedprocess_namegroup_cpu_seconds_total Cpu user usage in seconds
# TYPE namedprocess_namegroup_cpu_seconds_total counter
)");

	const double clock_ticks_to_s = double(1) / sysconf(_SC_CLK_TCK);

	for (const auto &i : groups)
		os.Format("namedprocess_namegroup_cpu_seconds_total{groupname=\"%s\",mode=\"system\"} %e\n"
			  "namedprocess_namegroup_cpu_seconds_total{groupname=\"%s\",mode=\"user\"} %e\n",
			  i.first.c_str(), i.second.stime * clock_ticks_to_s,
			  i.first.c_str(), i.second.utime * clock_ticks_to_s);

	os.Write(R"(# HELP namedprocess_namegroup_memory_bytes number of bytes of memory in use
# TYPE namedprocess_namegroup_memory_bytes gauge
)");

	const size_t page_size = sysconf(_SC_PAGESIZE);

	for (const auto &i : groups)
		os.Format("namedprocess_namegroup_memory_bytes{groupname=\"%s\",memtype=\"resident\"} %lu\n"
			  "namedprocess_namegroup_memory_bytes{groupname=\"%s\",memtype=\"virtual\"} %lu\n",
			  i.first.c_str(), i.second.vsize,
			  i.first.c_str(), i.second.rss * page_size);

	os.Write(R"(# HELP namedprocess_namegroup_minor_page_faults_total Minor page faults
# TYPE namedprocess_namegroup_minor_page_faults_total counter
)");

	for (const auto &i : groups)
		os.Format("namedprocess_namegroup_minor_page_faults_total{groupname=\"%s\"} %lu\n",
			  i.first.c_str(), i.second.minflt);

	os.Write(R"(# HELP namedprocess_namegroup_num_procs number of processes in this group
# TYPE namedprocess_namegroup_num_procs gauge
)");

	for (const auto &i : groups)
		os.Format("namedprocess_namegroup_num_procs{groupname=\"%s\"} %u\n",
			  i.first.c_str(), i.second.n_procs);

	os.Write(R"(# HELP namedprocess_namegroup_num_threads Number of threads
# TYPE namedprocess_namegroup_num_threads gauge
)");

	for (const auto &i : groups)
		os.Format("namedprocess_namegroup_num_threads{groupname=\"%s\"} %u\n",
			  i.first.c_str(), i.second.n_threads);
}

static void
ExportProc(const ProcessExporterConfig &config, BufferedOutputStream &os,
	   FileDescriptor proc_fd)
{
	DumpProcessGroups(os, CollectProcessGroups(config, proc_fd));
}

static void
ExportProc(const ProcessExporterConfig &config, BufferedOutputStream &os)
{
	ExportProc(config, os, OpenDirectory("/proc"));
}

int
main(int argc, char **argv) noexcept
try {
	const char *config_file = "/etc/cm4all/prometheus-exporters/process.yml";
	if (argc >= 2)
		config_file = argv[1];

	if (argc > 2) {
		fprintf(stderr, "Usage: %s CONFIGFILE\n", argv[0]);
		return EXIT_FAILURE;
	}

	const auto config = LoadProcessExporterConfig(config_file);

	return RunExporter([&](BufferedOutputStream &os){
		ExportProc(config, os);
	});
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
