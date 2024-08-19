// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "EFrontend.hxx"
#include "event/Loop.hxx"

#include <systemd/sd-daemon.h>

EFrontend::EFrontend(EventLoop &event_loop, PrometheusExporterHandler &handler)
{
	int n_listeners = sd_listen_fds(true);
	if (n_listeners < 0)
		throw std::runtime_error{"No systemd sockets"};

	for (int i = 0; i < n_listeners; ++i)
		listeners.emplace_front(event_loop,
					UniqueSocketDescriptor{SD_LISTEN_FDS_START + i},
					handler);
}

int
EFrontend::Run(EventLoop &event_loop) noexcept
{
	/* tell systemd we're ready */
	sd_notify(0, "READY=1");

	event_loop.Run();

	return EXIT_SUCCESS;
}
