// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "io/StdioOutputStream.hxx"
#include "io/StringOutputStream.hxx"
#include "io/BufferedOutputStream.hxx"
#include "lib/zlib/GzipOutputStream.hxx"
#include "util/Concepts.hxx"
#include "util/PrintException.hxx"
#include "util/ScopeExit.hxx"
#include "util/SpanCast.hxx"

#include <systemd/sd-daemon.h>

#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

template<typename T>
concept Handler = Invocable<T, BufferedOutputStream &>;

int
RunExporterStdio(Handler auto handler)
{
	int result = EXIT_SUCCESS;

	StdioOutputStream sos(stdout);
	BufferedOutputStream bos(sos);

	try {
		handler(bos);
	} catch (...) {
		PrintException(std::current_exception());
		result = EXIT_FAILURE;
	}

	bos.Flush();

	return result;
}

struct FrontendRequest {
	bool valid = false;

	bool gzip = false;
};

FrontendRequest
ReceiveFrontendRequest(int fd) noexcept;

bool
SendResponse(int fd, bool gzip, std::span<const std::byte> body) noexcept;

int
RunExporterHttp(const std::size_t n_listeners, Handler auto handler)
{
	int result = EXIT_SUCCESS;

	auto pfds = std::make_unique<struct pollfd[]>(n_listeners);
	for (std::size_t i = 0; i < n_listeners; ++i) {
		auto &pfd = pfds[i];
		pfd.fd = SD_LISTEN_FDS_START + i;
		pfd.events = POLLIN;
	}

	/* tell systemd we're ready */
	sd_notify(0, "READY=1");

	while (true) {
		if (poll(pfds.get(), n_listeners, -1) <= 0)
			break;

		for (std::size_t i = 0; i < n_listeners; ++i) {
			if (pfds[i].revents & (POLLERR | POLLHUP)) {
				pfds[i].fd = -1;
				pfds[i].revents = 0;
				continue;
			}

			int fd = accept4(SD_LISTEN_FDS_START + i,
					 nullptr, nullptr,
					 SOCK_CLOEXEC);
			if (fd < 0) {
				pfds[i].fd = -1;
				pfds[i].revents = 0;
				continue;
			}

			AtScopeExit(fd) { close(fd); };

			/* read the HTTP request (which appears to be
			   necessary to avoid ECONNRESET), but we
			   don't evaluate it; we just write the
			   response */
			const auto request = ReceiveFrontendRequest(fd);
			if (!request.valid)
				continue;

			try {
				StringOutputStream sos;

				if (request.gzip) {
					GzipOutputStream zos(sos);
					BufferedOutputStream bos(zos);
					handler(bos);
					bos.Flush();
					zos.Finish();
				} else  {
					BufferedOutputStream bos(sos);
					handler(bos);
					bos.Flush();
				}

				const auto &value = sos.GetValue();

				if (SendResponse(fd, request.gzip,
						 AsBytes(value)))
					/* this avoids resetting the
					   connection on close() */
					shutdown(fd, SHUT_WR);
			} catch (...) {
				PrintException(std::current_exception());
			}
		}
	}

	return result;
}

int
RunExporter(Handler auto handler)
{
	int n_listeners = sd_listen_fds(true);
	if (n_listeners > 0)
		/* if we have systemd sockets, assume those are HTTP
		   listeners */
		return RunExporterHttp(n_listeners, handler);

	return RunExporterStdio(handler);
}
