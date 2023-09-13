// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "io/DirectoryReader.hxx"
#include "io/Open.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "util/PrintException.hxx"

#include <concepts>
#include <cstdlib>
#include <exception>

void
ForEachProcess(FileDescriptor proc_fd,
	       std::invocable<unsigned, FileDescriptor> auto f)
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

void
ForEachProcessThread(FileDescriptor pid_fd,
		     std::invocable<unsigned, FileDescriptor> auto f)
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

void
ForEachThread(FileDescriptor proc_fd,
	      std::invocable<unsigned, FileDescriptor> auto f)
{
	ForEachProcess(proc_fd, [&](unsigned, FileDescriptor pid_fd){
		ForEachProcessThread(pid_fd, f);
	});
}
