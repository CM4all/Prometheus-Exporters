// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "CgroupConfig.hxx"
#include "Yaml.hxx"

#include <fnmatch.h>

bool
CgroupExporterConfig::CheckIgnoreName(const char *name) const noexcept
{
	for (const auto &i : ignore_names)
		if (fnmatch(i.c_str(), name, 0) == 0)
			return true;

	return false;
}

static auto
LoadCgroupExporterConfig(const YAML::Node &node)
{
	CgroupExporterConfig config;

	const auto ops = node["opaque_paths"];
	if (ops && ops.IsSequence())
		for (const auto &i : ops)
			config.opaque_paths.emplace(i.as<std::string>());

	const auto ins = node["ignore_names"];
	if (ins && ins.IsSequence())
		for (const auto &i : ins)
			config.ignore_names.emplace(i.as<std::string>());

	return config;
}

CgroupExporterConfig
LoadCgroupExporterConfig(const char *path)
{
	return LoadCgroupExporterConfig(YAML::LoadFile(path));
}
