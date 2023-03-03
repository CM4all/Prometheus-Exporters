// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "Syntax.hxx"

#include "util/CharUtil.hxx"

/**
 * @see https://prometheus.io/docs/instrumenting/writing_exporters/#naming
 */
static constexpr bool
IsMetricNameChar(char ch) noexcept
{
	return IsAlphaNumericASCII(ch) || ch == ':' || ch == '_';
}

[[gnu::pure]]
std::string
SanitizeMetricName(std::string_view src) noexcept
{
	std::string result;
	result.reserve(src.size());

	bool pending_underscore = false;
	for (const auto ch : src) {
		if (ch == '_' || !IsMetricNameChar(ch)) {
			pending_underscore = true;
			continue;
		}

		if (pending_underscore) {
			pending_underscore = false;
			result.push_back('_');
		}

		result.push_back(ch);
	}

	return result;
}
