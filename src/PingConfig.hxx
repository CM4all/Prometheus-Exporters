// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "net/IPv4Address.hxx"

#include <forward_list>

struct PingExporterConfig {
	std::forward_list<IPv4Address> addresses;
};

PingExporterConfig
LoadPingExporterConfig(const char *path);
