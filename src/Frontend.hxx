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

#pragma once

#include "io/StdioOutputStream.hxx"
#include "io/StringOutputStream.hxx"
#include "io/BufferedOutputStream.hxx"
#include "lib/zlib/GzipOutputStream.hxx"
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

template<typename Handler>
int
RunExporterStdio(Handler &&handler)
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

template<typename Handler>
int
RunExporterHttp(const std::size_t n_listeners, Handler &&handler)
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

template<typename Handler>
int
RunExporter(Handler &&handler)
{
	int n_listeners = sd_listen_fds(true);
	if (n_listeners > 0)
		/* if we have systemd sockets, assume those are HTTP
		   listeners */
		return RunExporterHttp(n_listeners,
				       std::forward<Handler>(handler));

	return RunExporterStdio(std::forward<Handler>(handler));
}
