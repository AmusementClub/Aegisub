// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include "automation_debug_service.h"

#include <string>

namespace Automation4 {
	struct AutomationDebugToggleResult {
		bool show_error = false;
		bool enabled = false;
		std::string message;
	};

	inline AutomationDebugToggleResult ToggleAutomationDebugService(AutomationDebugService *service)
	{
		if (!service)
			return {true, false, "Automation debug service is unavailable."};

		bool const was_enabled = service->IsEnabled();
		bool const enabled = service->Toggle();
		if (!enabled) {
			if (was_enabled)
				return {false, false, "Automation debug mode disabled"};
			return {true, false, "Failed to enable automation debug mode. Check the configured debug port and token settings."};
		}

		auto endpoint = service->GetEndpoint();
		if (endpoint.available) {
			return {
				false,
				true,
				"Automation debug mode enabled on " + endpoint.host + ":" + std::to_string(endpoint.port)
					+ (endpoint.token.empty() ? " without token" : "")
			};
		}

		return {false, true, "Automation debug mode enabled"};
	}
}
