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

#include "io/DirectoryReader.hxx"
#include "io/Open.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "util/PrintException.hxx"

#include <cstdlib>
#include <exception>

template<typename F>
void
ForEachProcess(FileDescriptor proc_fd, F &&f)
{
	DirectoryReader r(OpenDirectory(proc_fd, "."));
	while (auto name = r.Read()) {
		char *endptr;
		auto pid = std::strtoul(name, &endptr, 10);
		if (endptr == name || *endptr != 0 || pid <= 0)
			/* not a positive number */
			continue;

		UniqueFileDescriptor pid_fd;

		try {
			pid_fd = OpenDirectory(proc_fd, name);
		} catch (...) {
			PrintException(std::current_exception());
		}

		f(pid, std::move(pid_fd));
	}
}

template<typename F>
void
ForEachProcessThread(FileDescriptor pid_fd, F &&f)
{
	DirectoryReader r(OpenDirectory(pid_fd, "task"));
	while (auto name = r.Read()) {
		char *endptr;
		auto tid = std::strtoul(name, &endptr, 10);
		if (endptr == name || *endptr != 0 || tid <= 0)
			/* not a positive number */
			continue;

		UniqueFileDescriptor tid_fd;

		try {
			tid_fd = OpenDirectory(r.GetFileDescriptor(), name);
		} catch (...) {
			PrintException(std::current_exception());
		}

		f(tid, std::move(tid_fd));
	}
}

template<typename F>
void
ForEachThread(FileDescriptor proc_fd, F &&f)
{
	ForEachProcess(proc_fd, [&](unsigned, FileDescriptor pid_fd){
		ForEachProcessThread(pid_fd, f);
	});
}
