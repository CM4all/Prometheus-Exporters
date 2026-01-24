// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <max.kellermann@ionos.com>

#pragma once

#include <forward_list>
#include <string>

struct MultiExporterConfig {
	struct Source {
		std::string uri;
	};

	std::forward_list<Source> sources;
};

MultiExporterConfig
LoadMultiExporterConfig(const char *path);
