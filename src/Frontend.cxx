// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

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

	const std::string_view raw(buffer, nbytes);

	for (const auto line : IterableSplitString(raw, '\n')) {
		auto ae = StringAfterPrefixIgnoreCase(line, "accept-encoding:");
		if (ae.data() != nullptr) {
			if (http_list_contains(ae, "gzip"))
				request.gzip = true;
		}
	}

	return request;

}

static bool
SendFull(int fd, std::span<const std::byte> buffer) noexcept
{
	while (!buffer.empty()) {
		ssize_t nbytes = send(fd, buffer.data(), buffer.size(),
				      MSG_NOSIGNAL);
		if (nbytes <= 0)
			return false;

		buffer = buffer.subspan(nbytes);
	}

	return true;
}

bool
SendResponse(int fd, bool gzip, std::span<const std::byte> body) noexcept
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
			body.size());

	return SendFull(fd, AsBytes(std::string_view{headers, header_size})) &&
		SendFull(fd, body);
}
