// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <max.kellermann@ionos.com>

#include "CephDebugfs.hxx"
#include "NumberParser.hxx"
#include "io/BufferedOutputStream.hxx"
#include "io/DirectoryReader.hxx"
#include "io/SmallTextFile.hxx"
#include "util/IterableSplitString.hxx"
#include "util/NumberParser.hxx"
#include "util/PrintException.hxx"
#include "util/StringCompare.hxx"
#include "util/StringSplit.hxx"

#include <cstdlib>

#include <fcntl.h>

using std::string_view_literals::operator""sv;

static inline auto
ParseNS(std::string_view text) noexcept
{
	return ParseUint64(text) * 1e-9;
}

/**
 * Contents of the file "/sys/kernel/debug/ceph/X/mds_sessions".
 */
struct MdsSessions {
	/**
	 * The "name" mount option.
	 */
	std::string name;
};

/**
 * Load the contents of the file
 * "/sys/kernel/debug/ceph/X/mds_sessions".
 *
 * Throws on error.
 *
 * @parm file the "mds_sessions" file descriptor
 */
static void
LoadMdsSessions(MdsSessions &result, auto &&file)
{
	for (std::string_view line : IterableSmallTextFile<1024>(std::move(file))) {
		if (SkipPrefix(line, "name \""sv))
			result.name = Split(line, '"').first;
	}
}

static void
ExportCephSize(BufferedOutputStream &os, std::string_view fsid, std::string_view name,
	       std::string_view contents)
{
	// remove the header and the separator
	contents = Split(Split(contents, '\n').second, '\n').second;

	for (const auto line : IterableSplitString(contents, '\n')) {
		auto [item, values] = Split(line, ' ');
		if (item.empty())
			continue;

		auto [total, rest] = Split(StripLeft(values), ' ');

		// skip avg_sz
		rest = Split(StripLeft(rest), ' ').second;

		// skip min_sz
		rest = Split(StripLeft(rest), ' ').second;

		// skip max_sz
		rest = Split(StripLeft(rest), ' ').second;

		const auto total_sz = StripLeft(rest);

		if (!total_sz.empty())
			os.Fmt("ceph_metrics_size_bytes{{fsid={:?},name={:?},item={:?}}} {}\n",
			       fsid, name, item, total_sz);

		if (!total.empty())
			os.Fmt("ceph_metrics_size_count{{fsid={:?},name={:?},item={:?}}} {}\n",
			       fsid, name, item, total);
	}
}

/**
 * Export /sys/kernel/debug/ceph/.../metrics/caps
 */
static void
ExportCephCaps(BufferedOutputStream &os, std::string_view fsid, std::string_view name,
		   std::string_view contents)
{
	// skip the header labels
	contents = Split(contents, '\n').second;

	// skip the separator line
	contents = Split(contents, '\n').second;

	for (const auto line : IterableSplitString(contents, '\n')) {
		auto [item, values] = Split(line, ' ');
		if (item.empty())
			continue;

		const auto [total, rest1] = Split(StripLeft(values), ' ');
		const auto [miss, rest2] = Split(StripLeft(rest1), ' ');
		const auto [hit, rest3] = Split(StripLeft(rest2), ' ');

		if (!total.empty())
			os.Fmt("ceph_metrics_caps_total{{fsid={:?},name={:?},item={:?}}} {}\n",
			       fsid, name, item, total);

		if (!miss.empty())
			os.Fmt("ceph_metrics_caps_miss{{fsid={:?},name={:?},item={:?}}} {}\n",
			       fsid, name, item, miss);

		if (!hit.empty())
			os.Fmt("ceph_metrics_caps_hit{{fsid={:?},name={:?},item={:?}}} {}\n",
			       fsid, name, item, hit);
	}
}

/**
 * Export /sys/kernel/debug/ceph/.../metrics/counters (only available
 * in CM4all kernels).
 */
static void
ExportCephCounters(BufferedOutputStream &os, std::string_view fsid, std::string_view name,
		   std::string_view contents)
{
	// remove the header
	contents = Split(contents, '\n').second;

	for (const auto line : IterableSplitString(contents, '\n')) {
		const auto [item, values] = Split(line, ' ');
		if (item.empty())
			continue;

		const auto [count, rest1] = Split(values, ' ');
		if (!count.empty())
			os.Fmt("ceph_metrics_count{{fsid={:?},name={:?},item={:?}}} {}\n",
			       fsid, name, item, count);

		const auto [size_bytes, rest2] = Split(rest1, ' ');
		if (!size_bytes.empty())
			os.Fmt("ceph_metrics_size{{fsid={:?},name={:?},item={:?}}} {}\n",
			       fsid, name, item, size_bytes);

		const auto [wait_ns, rest3] = Split(rest2, ' ');
		if (!wait_ns.empty())
			os.Fmt("ceph_metrics_wait{{fsid={:?},name={:?},item={:?}}} {:e}\n",
			       fsid, name, item, ParseNS(wait_ns));
	}
}

void
ExportCeph(BufferedOutputStream &os)
{
	os.Write(R"(
# HELP ceph_metrics_size_bytes Bytes transferred to/from a Ceph server
# TYPE ceph_metrics_size_bytes counter
# HELP ceph_metrics_size_count Number of operations to/from a Ceph server
# TYPE ceph_metrics_size_count counter
# HELP ceph_metrics_caps_total Number of leases
# TYPE ceph_metrics_caps_total gauge
# HELP ceph_metrics_caps_miss Number of lease misses
# TYPE ceph_metrics_caps_miss counter
# HELP ceph_metrics_caps_hit Number of lease hits
# TYPE ceph_metrics_caps_hit counter
# HELP ceph_metrics_count Total number of operations on this Ceph mount
# TYPE ceph_metrics_count counter
# HELP ceph_metrics_size Total number of bytes on this Ceph mount
# TYPE ceph_metrics_size counter
# HELP ceph_metrics_wait Total number of seconds waited on this Ceph mount
# TYPE ceph_metrics_wait counter
)");

	UniqueFileDescriptor d;
	if (!d.Open("/sys/kernel/debug/ceph", O_DIRECTORY|O_RDONLY))
		return;

	DirectoryReader dr{std::move(d)};
	while (auto filename = dr.Read()) {
		const auto fsid = Split(std::string_view{filename}, '.').first;
		if (fsid.empty())
			continue;

		UniqueFileDescriptor subdir;
		if (!subdir.Open({dr.GetFileDescriptor(), filename}, O_DIRECTORY|O_PATH))
			continue;

		MdsSessions mds_sessions;

		try {
			LoadMdsSessions(mds_sessions, FileAt{subdir, "mds_sessions"});
		} catch (...) {
			PrintException(std::current_exception());
		}

		const std::string_view name = mds_sessions.name;

		UniqueFileDescriptor f;
		if (f.OpenReadOnly({subdir, "metrics/size"})) {
			WithSmallTextFile<4096>(f, [&os, fsid, name](std::string_view contents){
				ExportCephSize(os, fsid, name, contents);
			});

			f.Close();
		}

		if (f.OpenReadOnly({subdir, "metrics/caps"})) {
			WithSmallTextFile<4096>(f, [&os, fsid, name](std::string_view contents){
				ExportCephCaps(os, fsid, name, contents);
			});

			f.Close();
		}

		if (f.OpenReadOnly({subdir, "metrics/counters"})) {
			WithSmallTextFile<4096>(f, [&os, fsid, name](std::string_view contents){
				ExportCephCounters(os, fsid, name, contents);
			});

			f.Close();
		}
	}
}
