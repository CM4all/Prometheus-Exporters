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
#include "util/StringView.hxx"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

static std::string
ToString(StringView s) noexcept
{
	return {s.data, s.size};
}

static inline auto
ParseUserHz(StringView text)
{
	static const double user_hz_to_seconds = 1.0 / sysconf(_SC_CLK_TCK);
	return ParseUint64(text) * user_hz_to_seconds;
}

static int64_t
ReadUint64File(FileDescriptor directory_fd, const char *filename)
{
	char buffer[64];
	auto s = ReadTextFile(directory_fd, filename, buffer, sizeof(buffer));
	return ParseUint64(s);
}

gcc_pure
static double
ReadDoubleFile(FileDescriptor directory_fd, const char *filename,
	       double factor=1.0)
{
	char buffer[64];
	auto s = ReadTextFile(directory_fd, filename, buffer, sizeof(buffer));
	return ParseUint64(s) * factor;
}

template<typename F>
static void
ForEachTextLine(FileDescriptor directory_fd, const char *filename, F &&f)
{
	char buffer[4096];
	const auto s = ReadTextFile(directory_fd, filename, buffer, sizeof(buffer));

	for (StringView i : IterableSplitString(s, '\n')) {
		i.Strip();
		f(i);
	}
}

template<typename F>
static void
ForEachNameValue(FileDescriptor directory_fd, const char *filename, F &&f)
{
	ForEachTextLine(directory_fd, filename, [&f](StringView line){
		auto s = line.Split(' ');
		if (!s.first.empty() && !s.second.IsNull())
			f(s.first, s.second);
	});
}

struct CgroupCpuacctValues {
	double usage = -1, user = -1, system = -1;
};

struct CgroupMemoryValues {
	int64_t usage = -1, kmem_usage = -1, memsw_usage = -1;
	int64_t failcnt = -1, kmem_failfnt = -1, memsw_failcnt = -1;
	std::map<std::string, uint64_t> stat;
};

struct CgroupValues {
	CgroupCpuacctValues cpuacct;
	CgroupMemoryValues memory;
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

	void Dive(FileDescriptor parent_fd, const char *name);
	void HandleRegularFile(FileDescriptor parent_fd, const char *name);
	void DoWalk(UniqueFileDescriptor directory_fd);
};

enum class WalkDecision { IGNORE, DIRECTORY, REGULAR };

static WalkDecision
CheckWalkFile(FileDescriptor directory_fd, const char *name)
{
	if (*name == '.')
		return WalkDecision::IGNORE;

	struct stat st;
	if (fstatat(directory_fd.Get(), name, &st,
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
WalkContext::Dive(FileDescriptor parent_fd, const char *name)
{
	const size_t old_length = length;
	const size_t name_length = strlen(name);

	if (old_length + name_length + 2 >= sizeof(path))
		return;

	if (length > 0)
		path[length++] = '/';
	memcpy(path + length, name, name_length);
	length += name_length;
	path[length] = 0;

	DoWalk(OpenDirectory(parent_fd, name, O_NOFOLLOW));

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
WalkContext::HandleRegularFile(FileDescriptor parent_fd, const char *base)
{
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
		group.cpuacct.usage = ReadDoubleFile(parent_fd, base,
						     nano_factor);
	} else if (StringIsEqual(base, "cpuacct.stat")) {
		ForEachNameValue(parent_fd, base, [&group](auto name, auto value){
			if (name.Equals("user"))
				group.cpuacct.user = ParseUserHz(value);
			else if (name.Equals("system"))
				group.cpuacct.system = ParseUserHz(value);
		});
	} else if (StringIsEqual(base, "memory.usage_in_bytes")) {
		group.memory.usage = ReadUint64File(parent_fd, base);
	} else if (StringIsEqual(base, "memory.kmem.usage_in_bytes")) {
		group.memory.kmem_usage = ReadUint64File(parent_fd, base);
	} else if (StringIsEqual(base, "memory.memsw.usage_in_bytes")) {
		group.memory.memsw_usage = ReadUint64File(parent_fd, base);
	} else if (StringIsEqual(base, "memory.stat")) {
		ForEachNameValue(parent_fd, base, [&group](auto name, auto value){
			if (name.EndsWith("_limit"))
				/* skip hierarchical_memory_limit */
				return;

			group.memory.stat[ToString(name)] = ParseUint64(value);
		});
	}
}

void
WalkContext::DoWalk(UniqueFileDescriptor directory_fd)
{
	DirectoryReader r(std::move(directory_fd));

	const bool opaque = config.opaque_paths.find(path) != config.opaque_paths.end();

	while (auto name = r.Read()) {
		switch (CheckWalkFile(r.GetFileDescriptor(), name)) {
		case WalkDecision::IGNORE:
			break;

		case WalkDecision::DIRECTORY:
			if (!opaque && !config.CheckIgnoreName(name))
				Dive(r.GetFileDescriptor(), name);
			break;

		case WalkDecision::REGULAR:
			try {
				HandleRegularFile(r.GetFileDescriptor(), name);
			} catch (...) {
				PrintException(std::current_exception());
			}
			break;
		}
	}
}

static auto
CollectCgroup(const CgroupExporterConfig &config)
{
	CgroupsData data;

	for (const char *mnt : {"/sys/fs/cgroup/blkio", "/sys/fs/cgroup/cpuacct", "/sys/fs/cgroup/memory"}) {
		WalkContext ctx(config, data);

		try {
			ctx.DoWalk(OpenDirectory(FileDescriptor(AT_FDCWD), mnt));
		} catch (...) {
			PrintException(std::current_exception());
		}
	}

	return data;
}

static void
WriteCpuacct(BufferedOutputStream &os, const char *group, const char *type,
	     double value)
{
	if (value >= 0)
		os.Format("cgroup_cpu_usage{groupname=\"%s\",type=\"%s\"} %e\n",
			  group, type, value);
}

static void
WriteMemory(BufferedOutputStream &os, const char *group, const char *type,
	    int64_t value)
{
	if (value >= 0)
		os.Format("cgroup_memory_usage{groupname=\"%s\",type=\"%s\"} %" PRId64 "\n",
			  group, type, value);
}

static void
WriteMemory(BufferedOutputStream &os, const char *group, const char *type,
	    uint64_t value)
{
	os.Format("cgroup_memory_usage{groupname=\"%s\",type=\"%s\"} %" PRIu64 "\n",
		  group, type, value);
}

static void
DumpCgroup(BufferedOutputStream &os, const CgroupsData &data)
{
	os.Write(R"(# HELP cgroup_cpu_usage CPU usage in seconds
# TYPE cgroup_cpu_usage counter
)");

	for (const auto &i : data.groups) {
		const char *group = i.first.c_str();
		const auto &cpu = i.second.cpuacct;
		WriteCpuacct(os, group, "user", cpu.user);
		WriteCpuacct(os, group, "system", cpu.system);
		WriteCpuacct(os, group, "total", cpu.usage);
	}

	os.Write(R"(# HELP cgroup_memory_usage Memory usage in bytes
# TYPE cgroup_memory_usage gauge
)");

	for (const auto &i : data.groups) {
		const char *group = i.first.c_str();
		const auto &memory = i.second.memory;
		WriteMemory(os, group, "total", memory.usage);
		WriteMemory(os, group, "kmem.total", memory.kmem_usage);
		WriteMemory(os, group, "memsw.total", memory.memsw_usage);

		for (const auto &m : memory.stat)
			WriteMemory(os, group, m.first.c_str(), m.second);
	}

	os.Write(R"(# HELP cgroup_memory_failures Memory limit failures
# TYPE cgroup_memory_failures counter
)");

	for (const auto &i : data.groups) {
		const char *group = i.first.c_str();
		const auto &memory = i.second.memory;
		WriteMemory(os, group, "memory", memory.failcnt);
		WriteMemory(os, group, "kmem", memory.kmem_failfnt);
		WriteMemory(os, group, "memsw", memory.memsw_failcnt);
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
		fprintf(stderr, "Usage: %s CONFIGFILE\n", argv[0]);
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
