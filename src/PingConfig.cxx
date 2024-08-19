// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "PingConfig.hxx"
#include "Yaml.hxx"
#include "net/AllocatedSocketAddress.hxx"
#include "net/IPv4Address.hxx"
#include "net/Parser.hxx"
#include "lib/fmt/RuntimeError.hxx"

static auto
LoadPingExporterConfig(const YAML::Node &node)
{
	PingExporterConfig config;

	if (!node.IsSequence())
		throw std::runtime_error{"Configuration file must be sequence"};

	for (const auto &i : node) {
		const auto &s = i.as<std::string>();
		const auto address = ParseSocketAddress(s.c_str(), 0, false);
		if (address.GetFamily() != AF_INET)
			throw FmtRuntimeError("{:?} is not an IPv4 address", s);

		config.addresses.emplace_front(IPv4Address::Cast(address));
	}

	return config;
}

PingExporterConfig
LoadPingExporterConfig(const char *path)
{
	return LoadPingExporterConfig(YAML::LoadFile(path));
}
