// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "PingConfig.hxx"
#include "EFrontend.hxx"
#include "event/net/PingClient.hxx"
#include "event/net/PrometheusExporterHandler.hxx"
#include "event/Loop.hxx"
#include "time/Cast.hxx"
#include "util/PrintException.hxx"

#include <fmt/core.h>

#include <optional>

#include <arpa/inet.h> // for inet_ntop()

using std::string_view_literals::operator""sv;

struct PingTargetStats {
	uint_least64_t n_requests, n_replies, n_errors, n_timeouts;

	Event::Duration wait;
};

class PingTarget final : PingClientHandler {
	static constexpr Event::Duration interval = std::chrono::seconds{10};

	CoarseTimerEvent timer;

	Event::TimePoint start_time;

	std::optional<PingClient> ping_client;

	PingTargetStats stats{};

	const IPv4Address address;

	char name[32];

public:
	explicit PingTarget(EventLoop &event_loop, const IPv4Address &_address) noexcept
		:timer(event_loop, BIND_THIS_METHOD(OnTimer)),
		 address(_address)
	{
		inet_ntop(AF_INET, &address.GetAddress(), name, sizeof(name));

		timer.Schedule(std::chrono::seconds{1});
	}

	auto &GetEventLoop() const noexcept {
		return timer.GetEventLoop();
	}

	const char *GetName() const noexcept {
		return name;
	}

	const auto &GetStats() const noexcept {
		return stats;
	}

private:
	void OnTimer() noexcept {
		if (ping_client) {
			++stats.n_timeouts;
			ping_client.reset();
		}

		timer.Schedule(interval);

		++stats.n_requests;
		start_time = GetEventLoop().SteadyNow();

		PingClientHandler &handler = *this;
		ping_client.emplace(GetEventLoop(), handler);
		ping_client->Start(address);
	}

	// virtual methods from class PingClientHandler
	void PingResponse() noexcept override {
		ping_client.reset();
		++stats.n_replies;
		stats.wait += GetEventLoop().SteadyNow() - start_time;
	}

	void PingTimeout() noexcept override {
		ping_client.reset();
		++stats.n_timeouts;
	}

	void PingError([[maybe_unused]] std::exception_ptr error) noexcept override {
		ping_client.reset();
		++stats.n_errors;
	}
};

class PingExporter final : public PrometheusExporterHandler {
	std::forward_list<PingTarget> targets;

public:
	PingExporter(EventLoop &event_loop, const PingExporterConfig &config) noexcept;

	// virtual methods from class PrometheusExporterHandler
	std::string OnPrometheusExporterRequest() override {
		std::string result{R"(
# HELP ping_requests Number of ICMP "echo request" messages sent
# TYPE ping_requests counter
# HELP ping_replies Number of ICMP "echo reply" messages received
# TYPE ping_replies counter
# HELP ping_wait Total wait time for ICMP "echo reply" in seconds
# TYPE ping_wait counter
# HELP ping_errors Number of errors received instead of ICMP "echo reply"
# TYPE ping_errors counter
# HELP ping_timeouts Number of timeouts waiting for ICMP "echo reply"
# TYPE ping_timeouts counter
)"};

		for (const auto &i : targets) {
			const std::string_view name = i.GetName();
			const auto &stats = i.GetStats();
			result += fmt::format(R"(
ping_requests{{address={:?}}} {}
ping_replies{{address={:?}}} {}
ping_wait{{address={:?}}} {}
ping_errors{{address={:?}}} {}
ping_timeouts{{address={:?}}} {}
)",
					      name, stats.n_requests,
					      name, stats.n_replies,
					      name, ToFloatSeconds(stats.wait),
					      name, stats.n_errors,
					      name, stats.n_timeouts);
		}

		return result;
	}

	void OnPrometheusExporterError(std::exception_ptr error) noexcept override {
		PrintException(std::move(error));
		// TODO exit?
	}
};

inline
PingExporter::PingExporter(EventLoop &event_loop, const PingExporterConfig &config) noexcept
{
	for (const auto &i : config.addresses)
		targets.emplace_front(event_loop, i);
}

int
main(int argc, char **argv) noexcept
try {
	const char *config_file = "/etc/cm4all/prometheus-exporters/ping.yml";
	if (argc >= 2)
		config_file = argv[1];

	if (argc > 2) {
		fmt::print(stderr, "Usage: {} CONFIGFILE\n", argv[0]);
		return EXIT_FAILURE;
	}

	const auto config = LoadPingExporterConfig(config_file);

	EventLoop event_loop;
	PingExporter ping_exporter{event_loop, config};
	EFrontend frontend{event_loop, ping_exporter};
	return frontend.Run(event_loop);
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
