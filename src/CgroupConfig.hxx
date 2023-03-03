// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include <set>
#include <string>

struct CgroupExporterConfig {
	std::set<std::string> opaque_paths;

	std::set<std::string> ignore_names;

	[[gnu::pure]]
	bool CheckIgnoreName(const char *name) const noexcept;
};

CgroupExporterConfig
LoadCgroupExporterConfig(const char *path);
