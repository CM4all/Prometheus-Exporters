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
#include "MultiConfig.hxx"
#include "curl/Easy.hxx"
#include "curl/Multi.hxx"

#include <string>

struct SourceRequest {
	CurlEasy curl;

	std::string value;

	SourceRequest(const MultiExporterConfig::Source &source);

	void WriteTo(BufferedOutputStream &os) const;

private:
	static size_t CurlWriteFunction(char *ptr, size_t size, size_t nmemb,
					void *userdata) noexcept;
};

SourceRequest::SourceRequest(const MultiExporterConfig::Source &source)
{
	const char *url = source.uri.c_str();
	const char *unix_socket_path = nullptr;
	const char *abstract_unix_socket = nullptr;

	if (*url == '/') {
		unix_socket_path = url;
		url = "http://local-socket.dummy/";
	} else if (*url == '@') {
		abstract_unix_socket = url + 1;
		url = "http://abstract-socket.dummy/";
	}

	curl.SetURL(url);

	if (unix_socket_path != nullptr)
		curl.SetOption(CURLOPT_UNIX_SOCKET_PATH, unix_socket_path);

	if (abstract_unix_socket != nullptr)
		curl.SetOption(CURLOPT_ABSTRACT_UNIX_SOCKET,
			       abstract_unix_socket);

	curl.SetFailOnError();
	curl.SetWriteFunction(CurlWriteFunction, this);
	curl.SetNoProgress();
	curl.SetNoSignal();
}

void
SourceRequest::WriteTo(BufferedOutputStream &os) const
{
	os.Write(value.data(), value.size());
}

size_t
SourceRequest::CurlWriteFunction(char *ptr, size_t size, size_t nmemb,
				 void *userdata) noexcept
{
	auto &sr = *(SourceRequest *)userdata;
	size_t consumed = size * nmemb;
	sr.value.append(ptr, consumed);
	return consumed;
}

static void
ExportMulti(const MultiExporterConfig &config, BufferedOutputStream &os)
{
	CurlMulti multi;
	std::forward_list<SourceRequest> requests;

	auto e = requests.before_begin();
	for (const auto &i : config.sources) {
		e = requests.emplace_after(e, i);
		multi.Add(e->curl.Get());
	}

	while (multi.Perform())
		multi.Wait();

	while (auto msg = multi.InfoRead())
		if (msg->msg == CURLMSG_DONE)
			multi.Remove(msg->easy_handle);

	for (const auto &request : requests)
		request.WriteTo(os);
}

int
main(int argc, char **argv) noexcept
try {
	const char *config_file = "/etc/cm4all/prometheus-exporters/multi.yml";
	if (argc >= 2)
		config_file = argv[1];

	if (argc > 2) {
		fprintf(stderr, "Usage: %s CONFIGFILE\n", argv[0]);
		return EXIT_FAILURE;
	}

	const auto config = LoadMultiExporterConfig(config_file);

	return RunExporter([&](BufferedOutputStream &os){
		ExportMulti(config, os);
	});
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
