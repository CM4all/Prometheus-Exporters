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

#include "ProcessConfig.hxx"
#include "ProcessInfo.hxx"

#include <yaml-cpp/yaml.h>

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
