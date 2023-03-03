// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "lib/pcre/UniqueRegex.hxx"

#include <forward_list>
#include <set>
#include <string>
#include <vector>

struct ProcessInfo;

struct ProcessNameConfig {
	std::string name;

	std::set<std::string> comm;
	std::set<std::string> exe;
	std::forward_list<UniqueRegex> cmdline;

	bool Match(const ProcessInfo &info) const noexcept;
	std::string MakeName(const ProcessInfo &info) const noexcept;
};

struct ProcessExporterConfig {
	std::vector<ProcessNameConfig> process_names;

	std::string MakeName(const ProcessInfo &info) const noexcept;
};

ProcessExporterConfig
LoadProcessExporterConfig(const char *path);
