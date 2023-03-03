// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "ProcessConfig.hxx"
#include "ProcessInfo.hxx"
#include "Yaml.hxx"

bool
ProcessNameConfig::Match(const ProcessInfo &info) const noexcept
{
	if (!comm.empty()) {
		const auto i = comm.find(info.comm);
		if (i == comm.end())
			return false;
	}

	if (!exe.empty()) {
		const auto i = exe.find(info.exe);
		if (i == exe.end())
			return false;
	}

	const std::string_view c = info.cmdline;
	for (const auto &i : cmdline)
		if (!i.Match(c))
			return false;

	return true;
}

std::string
ProcessNameConfig::MakeName(const ProcessInfo &info) const noexcept
{
	if (!name.empty())
		return name;

	return info.exe;
}

std::string
ProcessExporterConfig::MakeName(const ProcessInfo &info) const noexcept
{
	for (const auto &i : process_names)
		if (i.Match(info))
			return i.MakeName(info);

	return {};
}

static auto
LoadProcessNameConfig(const YAML::Node &node)
{
	ProcessNameConfig pn;

	const auto name = node["name"];
	if (name && name.IsScalar())
		pn.name = name.as<std::string>();

	const auto comm = node["comm"];
	if (comm && comm.IsSequence())
		for (const auto &e : comm)
			pn.comm.emplace(e.as<std::string>());

	const auto exe = node["exe"];
	if (exe && exe.IsSequence())
		for (const auto &e : exe)
			pn.exe.emplace(e.as<std::string>());

	const auto cmdline = node["cmdline"];
	if (cmdline && cmdline.IsSequence())
		for (const auto &c : cmdline)
			pn.cmdline.emplace_front(c.as<std::string>().c_str(),
						 false, false);

	return pn;
}

static auto
LoadProcessExporterConfig(const YAML::Node &node)
{
	ProcessExporterConfig config;

	const auto pns = node["process_names"];
	if (!pns || !pns.IsSequence())
		throw std::runtime_error("Sequence 'process_names' expected");

	for (const auto &i : pns) {
		if (!i.IsMap())
			throw std::runtime_error("Map expected");

		config.process_names.emplace_back(LoadProcessNameConfig(i));
	}

	return config;
}

ProcessExporterConfig
LoadProcessExporterConfig(const char *path)
{
	return LoadProcessExporterConfig(YAML::LoadFile(path));
}
