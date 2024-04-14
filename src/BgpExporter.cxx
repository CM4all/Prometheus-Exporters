// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "Frontend.hxx"
#include "system/Error.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "util/AllocatedArray.hxx"
#include "util/PrintException.hxx"
#include "util/ScopeExit.hxx"

#include <nlohmann/json.hpp>

#include <signal.h>
#include <spawn.h>

using std::string_view_literals::operator""sv;

static UniqueFileDescriptor
SpawnStdoutPipe(char *const* argv, char *const* envp)
{
	UniqueFileDescriptor stdout_r, stdout_w;
	if (!UniqueFileDescriptor::CreatePipe(stdout_r, stdout_w))
		throw MakeErrno("Failed to create pipe");

	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	AtScopeExit(&attr) { posix_spawnattr_destroy(&attr); };

	sigset_t signals;
	sigemptyset(&signals);
	posix_spawnattr_setsigmask(&attr, &signals);
	sigaddset(&signals, SIGCHLD);
	sigaddset(&signals, SIGHUP);
	posix_spawnattr_setsigdefault(&attr, &signals);

	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF|POSIX_SPAWN_SETSIGMASK);

	posix_spawn_file_actions_t file_actions;
	posix_spawn_file_actions_init(&file_actions);
	AtScopeExit(&file_actions) { posix_spawn_file_actions_destroy(&file_actions); };

        posix_spawn_file_actions_adddup2(&file_actions, stdout_w.Get(), STDOUT_FILENO);

	pid_t pid;
	if (posix_spawn(&pid, argv[0], &file_actions, &attr, argv, envp) != 0)
		throw MakeErrno("Failed to execute process");

	return stdout_r;
}

static AllocatedArray<char>
SpawnReadString(char *const* argv, char *const* envp, std::size_t max_length)
{
	const auto fd = SpawnStdoutPipe(argv, envp);

	AllocatedArray<char> buffer{max_length + 1};
	std::size_t fill = 0;

	while (true) {
		std::span<char> dest{buffer};
		dest = dest.subspan(fill);

		auto nbytes = fd.Read(std::as_writable_bytes(dest));
		if (nbytes < 0)
			throw MakeErrno("Failed to read from pipe");

		if (nbytes == 0) {
			buffer.SetSize(fill);
			return buffer;
		}

		fill += static_cast<std::size_t>(nbytes);
		if (fill > max_length)
			throw std::runtime_error{"Too much output from child process"};
	}
}

static nlohmann::json
SpawnReadJson(char *const* argv, char *const* envp, std::size_t max_length)
{
	const auto buffer = SpawnReadString(argv, envp, max_length);
	const std::span<const char> string{buffer};
	return nlohmann::json::parse(ToStringView(string));
}

static constexpr int
NeighborStateToInteger(std::string_view state) noexcept
{
	if (state == "Idle"sv)
		return 0;
	else if (state == "Connect"sv)
		return 1;
	else if (state == "Active"sv)
		return 2;
	else if (state == "OpenSent"sv)
		return 3;
	else if (state == "OpenConfirm"sv)
		return 4;
	else if (state == "Established"sv)
		return 5;
	else
		return -1;
}

static void
ExportNeighborStats(BufferedOutputStream &os,
		    const std::string_view remote_as,
		    const std::string_view description,
		    const std::string_view remote_addr,
		    const nlohmann::json &stats)
{
	if (const auto prefixes = stats.find("prefixes"sv); prefixes != stats.end()) {
		os.Fmt("obgpd_peer_prefixes_advertised{{remote_as={:?},description={:?},remote_addr={:?}}} {}\n",
		       remote_as, description, remote_addr,
		       prefixes->at("sent"sv).get<uint_least32_t>());
		os.Fmt("obgpd_peer_prefixes_received{{remote_as={:?},description={:?},remote_addr={:?}}} {}\n",
		       remote_as, description, remote_addr,
		       prefixes->at("received"sv).get<uint_least32_t>());
	}

	if (const auto message = stats.find("message"sv); message != stats.end()) {
		os.Fmt("obgpd_peer_messages_sent{{remote_as={:?},description={:?},remote_addr={:?}}} {}\n",
		       remote_as, description, remote_addr,
		       message->at("sent"sv).at("total"sv).get<uint_least32_t>());
		os.Fmt("obgpd_peer_messages_received{{remote_as={:?},description={:?},remote_addr={:?}}} {}\n",
		       remote_as, description, remote_addr,
		       message->at("received"sv).at("total"sv).get<uint_least32_t>());
	}

	if (const auto update = stats.find("update"sv); update != stats.end()) {
		const auto &sent = update->at("sent"sv);
		os.Fmt("obgpd_peer_updates_sent{{remote_as={:?},description={:?},remote_addr={:?}}} {}\n",
		       remote_as, description, remote_addr,
		       sent.at("updates"sv).get<uint_least32_t>() +
		       sent.at("withdraws"sv).get<uint_least32_t>());

		const auto &received = update->at("received"sv);
		os.Fmt("obgpd_peer_updates_received{{remote_as={:?},description={:?},remote_addr={:?}}} {}\n",
		       remote_as, description, remote_addr,
		       received.at("updates"sv).get<uint_least32_t>() +
		       received.at("withdraws"sv).get<uint_least32_t>());
	}

	// TODO route-refresh?
}

static void
ExportNeighbor(BufferedOutputStream &os, const nlohmann::json &neighbor)
{
	std::string_view remote_as = neighbor.at("remote_as"sv).get<std::string_view>();
	std::string_view description = neighbor.at("description"sv).get<std::string_view>();
	std::string_view remote_addr = neighbor.at("remote_addr"sv).get<std::string_view>();

	if (const auto i = neighbor.find("last_updown_sec"sv); i != neighbor.end())
		os.Fmt("obgpd_peer_time{{remote_as={:?},description={:?},remote_addr={:?}}} {}\n",
		       remote_as, description, remote_addr,
		       i->get<uint_least32_t>());

	if (const auto i = neighbor.find("state"sv); i != neighbor.end())
		os.Fmt("obgpd_peer_state{{remote_as={:?},description={:?},remote_addr={:?}}} {}\n",
		       remote_as, description, remote_addr,
		       NeighborStateToInteger(i->get<std::string_view>()));

	if (const auto stats = neighbor.find("stats"sv); stats != neighbor.end())
		ExportNeighborStats(os, remote_as, description, remote_addr, *stats);
}

static void
ExportNeighbors(BufferedOutputStream &os, const nlohmann::json &neighbors)
{
	if (!neighbors.is_array())
		return;

	for (const auto &i : neighbors)
		ExportNeighbor(os, i);
}

static void
ExportBgp(BufferedOutputStream &os)
{
	static constexpr const char *argv[] = {
		"/usr/sbin/bgpctl", "-j", "show", "neighbor", nullptr
	};

	const auto j = SpawnReadJson(const_cast<char *const *>(argv), nullptr, 256 * 1024);

	/* this exporter imitates the protocol of
	   https://framagit.org/ledeuns/obgpd_exporter */
	os.Write(R"(# obgpd_peer_time Seconds since last neighbor state change
# TYPE obgpd_peer_time gauge
# HELP obgpd_peer_state State of a neighbor (-1 = Unknown, 0 = Idle, 1 = Connect, 2 = Active, 3 = OpenSent, 4 = OpenConfirm, 5 = Established).
# TYPE obgpd_peer_state gauge
# HELP obgpd_peer_prefixes_advertised Number of prefixes advertised to a neighbor
# TYPE obgpd_peer_prefixes_advertised gauge
# HELP obgpd_peer_prefixes_received Number of prefixes received from a neighbor
# TYPE obgpd_peer_prefixes_received gauge
# HELP obgpd_peer_messages_sent Number of BGP messages sent to a neighbor
# TYPE obgpd_peer_messages_sent gauge
# HELP obgpd_peer_messages_received Number of BGP messages received from a neighbor
# TYPE obgpd_peer_messages_received gauge
# HELP obgpd_peer_updates_sent Number of BGP updates/withdraw sent to a neighbor
# TYPE obgpd_peer_updates_sent gauge
# HELP obgpd_peer_updates_received Number of BGP updates/withdraw received from a neighbor
# TYPE obgpd_peer_updates_received gauge
)");

	if (const auto i = j.find("neighbors"sv); i != j.end())
		ExportNeighbors(os, *i);
}

int
main(int argc, char **argv) noexcept
try {
	if (argc > 1) {
		fmt::print(stderr, "Usage: {}\n", argv[0]);
		return EXIT_FAILURE;
	}

	/* auto-reap zombie processes */
	signal(SIGCHLD, SIG_IGN);

	return RunExporter(ExportBgp);
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
