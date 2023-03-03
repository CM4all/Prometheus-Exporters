// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include <string>
#include <string_view>

/**
 * Replace any disallowed characters with underscore and return a
 * sanitized string.
 */
[[gnu::pure]]
std::string
SanitizeMetricName(std::string_view s) noexcept;
