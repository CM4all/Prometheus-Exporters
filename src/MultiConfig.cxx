// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "MultiConfig.hxx"
#include "Yaml.hxx"

#include <stdexcept>

static auto
LoadSource(const YAML::Node &node)
{
	MultiExporterConfig::Source source;
	source.uri = node.as<std::string>();
	return source;
}

static auto
LoadMultiExporterConfig(const YAML::Node &node)
{
	MultiExporterConfig config;

	const auto e = node["sources"];
	if (!e)
		throw std::runtime_error("No 'sources'");

	if (!e.IsSequence())
		throw std::runtime_error("'sources' must be a sequence");

	auto a = config.sources.before_begin();
	for (const auto &i : e)
		a = config.sources.emplace_after(a, LoadSource(i));

	return config;
}

MultiExporterConfig
LoadMultiExporterConfig(const char *path)
{
	return LoadMultiExporterConfig(YAML::LoadFile(path));
}
