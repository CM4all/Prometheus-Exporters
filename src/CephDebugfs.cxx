// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <max.kellermann@ionos.com>

#include "CephDebugfs.hxx"
#include "NumberParser.hxx"
#include "io/BufferedOutputStream.hxx"
#include "io/BufferedReader.hxx"
#include "io/FdReader.hxx"
#include "io/DirectoryReader.hxx"
#include "io/Open.hxx"
#include "io/SmallTextFile.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "util/IterableSplitString.hxx"
#include "util/NumberParser.hxx"
#include "util/PrintException.hxx"
#include "util/StaticVector.hxx"
#include "util/StringCompare.hxx"
#include "util/StringSplit.hxx"

#include <array>
#include <cstdlib>
#include <string>

#include <fcntl.h>

using std::string_view_literals::operator""sv;

static constexpr std::size_t CEPH_MAX_MDS = 0x400;

static inline auto
ParseNS(std::string_view text) noexcept
{
	return ParseUint64(text) * 1e-9;
}

/**
 * Contents of the file "/sys/kernel/debug/ceph/X/mds_sessions".
 */
struct CephMds {
	std::string global_id;

	/**
	 * The "name" mount option.
	 */
	std::string name;

	struct OneMds {
		std::string protocol;
		std::string address;
		std::string state;
		std::string session_state;

		bool IsDefined() const noexcept {
			return !protocol.empty() || !address.empty() || !state.empty() || !session_state.empty();
		}
	};

	StaticVector<OneMds, CEPH_MAX_MDS> list;

	bool blocklisted = false;

	OneMds *MakeMds(std::size_t id) noexcept {
		if (id >= list.max_size())
			return nullptr;

		while (list.size() <= id)
			list.emplace_back();

		return &list[id];
	}
};

/**
 * Load the contents of the file
 * "/sys/kernel/debug/ceph/X/status".
 *
 * Throws on error.
 *
 * @parm file the "status" file descriptor
 */
static void
LoadStatus(CephMds &result, auto &&file)
{
	for (std::string_view line : IterableSmallTextFile<1024>(std::move(file))) {
		if (SkipPrefix(line, "blocklisted: "sv)) {
			result.blocklisted = line != "false"sv;
		}
	}
}

/**
 * Load the contents of the file
 * "/sys/kernel/debug/ceph/X/mdsmap".
 *
 * Throws on error.
 *
 * @parm file the "mdsmap" file descriptor
 */
static void
LoadMdsMap(CephMds &result, auto &&file)
{
	for (std::string_view line : IterableSmallTextFile<4096>(std::move(file))) {
		if (SkipPrefix(line, "\tmds"sv)) {
			const auto [id_string, rest1] = Split(line, '\t');
			if (id_string.empty())
				continue;

			auto [address, rest2] = Split(rest1, '\t');
			auto [state, _] = Split(rest2, '\t');

			unsigned id;
			if (!ParseIntegerTo(id_string, id))
				continue;

			auto *mds = result.MakeMds(id);
			if (mds == nullptr)
				continue;

			if (address.starts_with('(')) {
				const auto [protocol, address2] = Split(address.substr(1), ')');
				mds->protocol = protocol;
				mds->address = address2;
			} else {
				mds->address = address;
			}

			if (state.starts_with('(') && state.ends_with(')'))
				state = state.substr(1, state.size() - 2);

			mds->state = state;
		}
	}
}

/**
 * Load the contents of the file
 * "/sys/kernel/debug/ceph/X/mds_sessions".
 *
 * Throws on error.
 *
 * @parm file the "mds_sessions" file descriptor
 */
static void
LoadMdsSessions(CephMds &result, auto &&file)
{
	for (std::string_view line : IterableSmallTextFile<1024>(std::move(file))) {
		if (SkipPrefix(line, "name \""sv))
			result.name = Split(line, '"').first;
		else if (SkipPrefix(line, "global_id "sv))
			result.global_id = line;
		else if (SkipPrefix(line, "mds."sv)) {
			const auto [id_string, session_state] = Split(line, ' ');

			unsigned id;
			if (!ParseIntegerTo(id_string, id))
				continue;

			auto *mds = result.MakeMds(id);
			if (mds == nullptr)
				continue;

			mds->session_state = session_state;
		}
	}
}

static constexpr std::array ceph_mds_ops{
	"lookup"sv,
	"lookuphash"sv,
	"lookupparent"sv,
	"lookupino"sv,
	"lookupname"sv,
	"getattr"sv,
	"getvxattr"sv,
	"setxattr"sv,
	"setattr"sv,
	"rmxattr"sv,
	"setlayou"sv,
	"setdirlayout"sv,
	"readdir"sv,
	"mknod"sv,
	"link"sv,
	"unlink"sv,
	"rename"sv,
	"mkdir"sv,
	"rmdir"sv,
	"symlink"sv,
	"create"sv,
	"open"sv,
	"lookupsnap"sv,
	"lssnap"sv,
	"mksnap"sv,
	"rmsnap"sv,
	"renamesnap"sv,
	"setfilelock"sv,
	"getfilelock"sv,
};

static std::size_t
ParseMdsOpName(std::string_view s) noexcept
{
	const auto it = std::find(ceph_mds_ops.begin(), ceph_mds_ops.end(), s);
	return std::distance(ceph_mds_ops.begin(), it);
}

struct CephMdsc {
	struct PerOp {
		uint64_t count;
	};

	struct PerMds {
		std::array<PerOp, std::size(ceph_mds_ops) + 1> per_op;
	};

	static constexpr std::size_t UNKNOWN_MDS = CEPH_MAX_MDS;
	static constexpr std::size_t NO_REQUEST = UNKNOWN_MDS + 1;
	static constexpr std::size_t NO_SESSION = NO_REQUEST + 1;

	std::array<PerMds, CEPH_MAX_MDS + 3> per_mds{};
};

static void
LoadMdsc(BufferedReader &r, CephMdsc &result)
{
	char *line;
	while ((line = r.ReadLine()) != nullptr) {
		const auto [tid, rest1] = Split(std::string_view{line}, '\t');
		const auto [mds_name, rest2] = Split(rest1, '\t');
		const auto [op_name, rest3] = Split(rest2, '\t');

		std::size_t mds_rank;
		if (const auto mds_rank_s = StringAfterPrefix(mds_name, "mds"sv);
		    !mds_rank_s.empty()) {
			if (!ParseIntegerTo(mds_rank_s, mds_rank) || mds_rank >= CEPH_MAX_MDS)
				mds_rank = CephMdsc::UNKNOWN_MDS;
		} else if (mds_name == "(no request)"sv)
			mds_rank = CephMdsc::NO_REQUEST;
		else if (mds_name == "(no session)"sv)
			mds_rank = CephMdsc::NO_SESSION;
		else
			mds_rank = CephMdsc::UNKNOWN_MDS;

		std::size_t op = ParseMdsOpName(op_name);

		++result.per_mds[mds_rank].per_op[op].count;
	}
}

static bool
LoadMdsc(FileAt file, CephMdsc &result)
{
	UniqueFileDescriptor fd;
	if (!fd.OpenReadOnly(file))
		return false;

	FdReader reader{fd};
	BufferedReader buffered_reader{reader};

	LoadMdsc(buffered_reader, result);
	return true;
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
# HELP ceph_client_blocklisted Is this Ceph client blocklisted?
# TYPE ceph_client_blocklisted gauge
# HELP ceph_mds Information about each MDS
# TYPE ceph_mds gauge
# HELP ceph_mds_pending_requests Number of pending MDS requests
# TYPE ceph_mds_pending_requests gauge
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

		CephMds mds_sessions;

		try {
			LoadStatus(mds_sessions, FileAt{subdir, "status"});
		} catch (...) {
			PrintException(std::current_exception());
		}

		try {
			LoadMdsMap(mds_sessions, FileAt{subdir, "mdsmap"});
		} catch (...) {
			PrintException(std::current_exception());
		}

		try {
			LoadMdsSessions(mds_sessions, FileAt{subdir, "mds_sessions"});
		} catch (...) {
			PrintException(std::current_exception());
		}

		const std::string_view name = mds_sessions.name;

		os.Fmt("ceph_client_blocklisted{{fsid={:?},name={:?}"sv, fsid, name);

		if (!mds_sessions.global_id.empty())
			os.Fmt(",global_id={:?}"sv, mds_sessions.global_id);

		os.Fmt("}} {}\n"sv, mds_sessions.blocklisted ? "1"sv : "0"sv);

		for (std::size_t id = 0; id < mds_sessions.list.size(); ++id) {
			const auto &mds = mds_sessions.list[id];
			if (!mds.IsDefined())
				continue;

			os.Fmt("ceph_mds{{fsid={:?},name={:?},mds=\"{}\""sv, fsid, name, id);

			if (!mds.protocol.empty())
				os.Fmt(",protocol={:?}"sv, mds.protocol);

			if (!mds.address.empty())
				os.Fmt(",address={:?}"sv, mds.address);

			if (!mds.state.empty())
				os.Fmt(",state={:?}"sv, mds.state);

			if (!mds.session_state.empty())
				os.Fmt(",session_state={:?}"sv, mds.session_state);

			os.Write("} 1\n"sv);
		}

		if (CephMdsc mdsc; LoadMdsc({subdir, "mdsc"}, mdsc)) {
			for (std::size_t rank = 0; rank < mds_sessions.list.size(); ++rank) {
				const auto &mdsc_rank = mdsc.per_mds[rank];

				std::string_view mds_address;

				if (rank < mds_sessions.list.size()) {
					const auto &mds = mds_sessions.list[rank];
					if (mds.IsDefined())
						mds_address = mds.address;
				}

				for (std::size_t op = 0; op < mdsc_rank.per_op.size(); ++op) {
					const auto &per_op = mdsc_rank.per_op[op];
					if (per_op.count == 0)
						continue;

					const std::string_view op_name = op < ceph_mds_ops.size()
						? ceph_mds_ops[op]
						: "unknown"sv;

					os.Fmt("ceph_mds_pending_requests{{fsid={:?},name={:?}"sv,
					       fsid, name);

					switch (rank) {
					case CephMdsc::UNKNOWN_MDS:
						os.Write(",mds=\"unkown\""sv);
						break;

					case CephMdsc::NO_REQUEST:
						os.Write(",mds=\"no_request\""sv);
						break;

					case CephMdsc::NO_SESSION:
						os.Write(",mds=\"no_session\""sv);
						break;

					default:
						os.Fmt(",mds=\"{}\""sv, rank);
						break;
					}

					if (!mds_address.empty())
						os.Fmt(",address={:?}"sv, mds_address);

					os.Fmt(",op={:?}}} {}\n",
					       op_name, per_op.count);
				}
			}
		}

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
