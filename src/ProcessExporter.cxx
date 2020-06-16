/*
 * Copyright 2020 CM4all GmbH
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
#include "ProcessConfig.hxx"
#include "ProcessInfo.hxx"
#include "ProcessIterator.hxx"
#include "io/BufferedOutputStream.hxx"
#include "io/DirectoryReader.hxx"
#include "io/Open.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "system/Error.hxx"
#include "util/CharUtil.hxx"
#include "util/Compiler.h"
#include "util/IterableSplitString.hxx"
#include "util/PrintException.hxx"

#include <cstdlib>
#include <string_view>

static std::string_view
ReadTextFile(FileDescriptor fd, char *buffer, size_t buffer_size)
{
	ssize_t nbytes = fd.Read(buffer, buffer_size);
	if (nbytes < 0)
		throw MakeErrno("Failed to read");

	const size_t size = nbytes;

	if (size >= buffer_size)
		throw std::runtime_error("File too large");

	buffer[size] = 0;
	return {buffer, size};
}

static std::string_view
ReadTextFile(FileDescriptor directory_fd, const char *filename,
	     char *buffer, size_t buffer_size)
{
	return ReadTextFile(OpenReadOnly(directory_fd, filename),
			    buffer, buffer_size);
}

template<typename T>
gcc_pure
static T
ParseUnsignedT(StringView text) noexcept
{
	text.StripLeft();

	T value = 0;
	for (char ch : text) {
		if (!IsDigitASCII(ch))
			break;

		value = value * T(10) + T(ch - '0');
	}

	return value;
}

gcc_pure
static auto
ParseUnsigned(StringView text) noexcept
{
	return ParseUnsignedT<unsigned>(text);
}

gcc_pure
static auto
ParseUnsignedLong(StringView text) noexcept
{
	return ParseUnsignedT<unsigned long>(text);
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
		line.Strip();
		if (line.empty())
			continue;

		switch (line.front()) {
		case 'n':
			if (line.SkipPrefix("nonvoluntary_ctxt_switches:"))
				result.nonvoluntary_ctxt_switches = ParseUnsigned(line);
			break;

		case 'v':
			if (line.SkipPrefix("voluntary_ctxt_switches:"))
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
ParseProcessStat(StringView text)
{
	ProcessStat result;

	StringView s;

	std::tie(s, text) = text.Split(' '); // pid

	std::tie(s, text) = text.Split(' '); // comm
	if (!s.empty() && s.front() == '(' && s.back() == ')') {
		s.pop_front();
		s.pop_back();
	}

	result.comm = s;

	std::tie(s, text) = text.Split(' '); // state
	if (!s.empty())
		result.state = s.front();

	std::tie(s, text) = text.Split(' '); // ppid
	std::tie(s, text) = text.Split(' '); // pgrp
	std::tie(s, text) = text.Split(' '); // session
	std::tie(s, text) = text.Split(' '); // tty_nr
	std::tie(s, text) = text.Split(' '); // tpgid
	std::tie(s, text) = text.Split(' '); // flags

	std::tie(s, text) = text.Split(' '); // minflt
	result.minflt = ParseUnsignedLong(s);

	std::tie(s, text) = text.Split(' '); // cminflt

	std::tie(s, text) = text.Split(' '); // majflt
	result.majflt = ParseUnsignedLong(s);

	std::tie(s, text) = text.Split(' '); // cmajflt

	std::tie(s, text) = text.Split(' '); // utime
	result.utime = ParseUnsignedLong(s);

	std::tie(s, text) = text.Split(' '); // stime
	result.stime = ParseUnsignedLong(s);

	std::tie(s, text) = text.Split(' '); // cutime
	std::tie(s, text) = text.Split(' '); // cstime
	std::tie(s, text) = text.Split(' '); // priority
	std::tie(s, text) = text.Split(' '); // nice
	std::tie(s, text) = text.Split(' '); // num_threads
	std::tie(s, text) = text.Split(' '); // itrealvalue
	std::tie(s, text) = text.Split(' '); // starttime

	std::tie(s, text) = text.Split(' '); // vsize
	result.vsize = ParseUnsignedLong(s);

	std::tie(s, text) = text.Split(' '); // rss
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
	char status_buffer[4096];
	const auto status =
		ParseProcessStatus(ReadTextFile(pid_fd, "status",
						status_buffer,
						sizeof(status_buffer)));

	char stat_buffer[1024];
	const auto stat =
		ParseProcessStat(ReadTextFile(pid_fd, "stat",
					      stat_buffer,
					      sizeof(stat_buffer)));

	group += status;
	group += stat;
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

		StringView name(exe, rl);
		name.RemoveSuffix(" (deleted)");

		const char *slash = name.FindLast('/');
		if (slash != nullptr)
			name.MoveFront(slash + 1);
		if (name.empty())
			return;

		const std::string_view exe_sv = name;

		char stat_buffer[1024];
		const auto stat =
			ParseProcessStat(ReadTextFile(pid_fd, "stat",
						      stat_buffer,
						      sizeof(stat_buffer)));

		ProcessInfo info;
		info.comm = stat.comm;
		info.exe = exe_sv;

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
	if (argc != 2) {
		fprintf(stderr, "Usage: %s CONFIGFILE\n", argv[0]);
		return EXIT_FAILURE;
	}

	const auto config = LoadProcessExporterConfig(argv[1]);

	return RunExporter([&](BufferedOutputStream &os){
		ExportProc(config, os);
	});
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
