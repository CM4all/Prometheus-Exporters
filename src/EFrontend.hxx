// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "event/net/PrometheusExporterListener.hxx"

#include <forward_list>

class EFrontend {
	std::forward_list<PrometheusExporterListener> listeners;

public:
	EFrontend(EventLoop &event_loop, PrometheusExporterHandler &handler);

	EFrontend(const EFrontend &) = delete;
	EFrontend &operator=(const EFrontend &) = delete;

	int Run(EventLoop &event_loop) noexcept;
};
