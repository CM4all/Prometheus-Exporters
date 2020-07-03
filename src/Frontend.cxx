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
#include "http/List.hxx"
#include "util/IterableSplitString.hxx"
#include "util/StringCompare.hxx"

FrontendRequest
ReceiveFrontendRequest(int fd) noexcept
{
	FrontendRequest request;

	char buffer[8192];
	ssize_t nbytes = recv(fd, buffer, sizeof(buffer), 0);
	if (nbytes <= 0)
		return request;

	request.valid = true;

	const StringView raw(buffer, nbytes);

	for (const auto line : IterableSplitString(raw, '\n')) {
		auto ae = StringAfterPrefixIgnoreCase(line, "accept-encoding:");
		if (!ae.IsNull()) {
			if (http_list_contains(ae, "gzip"))
				request.gzip = true;
		}
	}

	return request;

}

static bool
SendFull(int fd, ConstBuffer<char> buffer) noexcept
{
	while (!buffer.empty()) {
		ssize_t nbytes = send(fd, buffer.data, buffer.size,
				      MSG_NOSIGNAL);
		if (nbytes <= 0)
			return false;

		buffer.skip_front(nbytes);
	}

	return true;
}

bool
SendResponse(int fd, bool gzip, ConstBuffer<char> body) noexcept
{
	char headers[1024];
	size_t header_size =
		sprintf(headers, "HTTP/1.1 200 OK\r\n"
			"connection: close\r\n"
			"%s"
			"content-type: text/plain\r\n"
			"content-length: %zu\r\n"
			"\r\n",
			gzip ? "content-encoding: gzip\r\n" : "",
			body.size);

	return SendFull(fd, {headers, header_size}) &&
		SendFull(fd, body);
}
