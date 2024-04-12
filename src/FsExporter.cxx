// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "Frontend.hxx"
#include "Syntax.hxx"
#include "NumberParser.hxx"
#include "Pressure.hxx"
#include "system/linux/listmount.h"
#include "system/linux/statmount.h"
#include "system/Error.hxx"
#include "io/BufferedOutputStream.hxx"
#include "io/DirectoryReader.hxx"
#include "io/SmallTextFile.hxx"
#include "util/IterableSplitString.hxx"
#include "util/PrintException.hxx"
#include "util/StringCompare.hxx"
#include "util/StringSplit.hxx"

#include <cstdlib>

#include <fcntl.h> // for AT_FDCWD
#include <linux/major.h> // for LOOP_MAJOR
#include <sys/stat.h> // for statx()
#include <sys/statfs.h> // for statfs()

#ifndef SB_RDONLY
#define SB_RDONLY 1
#endif

using std::string_view_literals::operator""sv;

static std::span<const __u64>
ListMount(__u64 mnt_id, std::span<__u64> buffer) noexcept
{
	const struct mnt_id_req req{
		.size = sizeof(req),
		.mnt_id = mnt_id,
	};

	int result = listmount(&req, buffer.data(), buffer.size(), 0);
	if (result < 0)
		return {};

	return buffer.first(result);
}

static bool
StatMount(__u64 mnt_id, __u64 param,
	  struct statmount *buf, std::size_t bufsize) noexcept
{

		const struct mnt_id_req req{
			.size = sizeof(req),
			.mnt_id = mnt_id,
			.param = param,
		};

		return do_statmount(&req, buf, bufsize, 0) == 0;
}

static void
ExportDiskUsage(BufferedOutputStream &os)
{
	os.Write(R"(# HELP node_filesystem_avail_bytes Filesystem space available to non-root users in bytes.
# TYPE node_filesystem_avail_bytes gauge
# HELP node_filesystem_files Filesystem total file nodes.
# TYPE node_filesystem_files gauge
# HELP node_filesystem_files_free Filesystem total free file nodes.
# TYPE node_filesystem_files_free gauge
# HELP node_filesystem_free_bytes Filesystem free space in bytes.
# TYPE node_filesystem_free_bytes gauge
# HELP node_filesystem_size_bytes Filesystem size in bytes.
# TYPE node_filesystem_size_bytes gauge
)");

	__u64 mnt_ids_buffer[256];
	const auto mnt_ids = ListMount(LSMT_ROOT, mnt_ids_buffer);

	for (const __u64 mnt_id : mnt_ids) {
		union {
			struct statmount statmount;
			std::byte raw[8192];
		} u;

		if (!StatMount(mnt_id,
			       STATMOUNT_SB_BASIC|STATMOUNT_MNT_POINT|STATMOUNT_FS_TYPE,
			       &u.statmount, sizeof(u)))
			continue;

		const char *const strings = reinterpret_cast<const char *>(&u.statmount + 1);
		const char *const fs_type = strings + u.statmount.fs_type;
		const char *const mnt_point = strings + u.statmount.mnt_point;

		if (u.statmount.sb_dev_major == LOOP_MAJOR ||
		    (u.statmount.sb_dev_major == 0 && !StringIsEqual(fs_type, "btrfs")) ||
		    u.statmount.sb_flags & SB_RDONLY)
			continue;

		struct statfs sfs;
		if (statfs(mnt_point, &sfs) < 0)
			continue;

		// TODO add the "device" attribute (statmount() doesn't provide it)

		os.Fmt(R"(node_filesystem_avail_bytes{{fstype={:?},mountpoint={:?}}} {}
node_filesystem_files{{fstype={:?},mountpoint={:?}}} {}
node_filesystem_files_free{{fstype={:?},mountpoint={:?}}} {}
node_filesystem_free_bytes{{fstype={:?},mountpoint={:?}}} {}
node_filesystem_size_bytes{{fstype={:?},mountpoint={:?}}} {}
)",
		       fs_type, mnt_point,
		       sfs.f_bavail * sfs.f_bsize,
		       fs_type, mnt_point,
		       sfs.f_files,
		       fs_type, mnt_point,
		       sfs.f_ffree,
		       fs_type, mnt_point,
		       sfs.f_bfree * sfs.f_bsize,
		       fs_type, mnt_point,
		       sfs.f_blocks * sfs.f_bsize);
	}
}

int
main(int argc, char **argv) noexcept
try {
	if (argc > 1) {
		fmt::print(stderr, "Usage: {}\n", argv[0]);
		return EXIT_FAILURE;
	}

	return RunExporter(ExportDiskUsage);
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
