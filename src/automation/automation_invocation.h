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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include <string>
#include <utility>

namespace Automation4 {
	enum class AutomationInvocationKind {
		MacroValidate,
		MacroRun,
		MacroIsActive,
		ExportFilterConfig,
		ExportFilterRun,
		Test,
	};

	struct AutomationInvocationCapabilities {
		bool allow_modify = false;
		bool allow_undo = false;
		bool allow_dialog = false;
	};

	struct AutomationInvocation {
		AutomationInvocationKind kind = AutomationInvocationKind::MacroRun;
		std::string feature_name;
		AutomationInvocationCapabilities capabilities;
	};

	inline const char* ToString(AutomationInvocationKind kind) noexcept {
		switch (kind) {
			case AutomationInvocationKind::MacroValidate: return "macro_validate";
			case AutomationInvocationKind::MacroRun: return "macro_run";
			case AutomationInvocationKind::MacroIsActive: return "macro_is_active";
			case AutomationInvocationKind::ExportFilterConfig: return "export_filter_config";
			case AutomationInvocationKind::ExportFilterRun: return "export_filter_run";
			case AutomationInvocationKind::Test: return "test";
		}
		return "unknown";
	}

	inline AutomationInvocation MakeMacroValidateInvocation(std::string feature_name) {
		return {
			AutomationInvocationKind::MacroValidate,
			std::move(feature_name),
			{false, false, false}
		};
	}

	inline AutomationInvocation MakeMacroRunInvocation(std::string feature_name) {
		return {
			AutomationInvocationKind::MacroRun,
			std::move(feature_name),
			{true, true, true}
		};
	}

	inline AutomationInvocation MakeMacroIsActiveInvocation(std::string feature_name) {
		return {
			AutomationInvocationKind::MacroIsActive,
			std::move(feature_name),
			{false, false, false}
		};
	}

	inline AutomationInvocation MakeExportFilterConfigInvocation(std::string feature_name) {
		return {
			AutomationInvocationKind::ExportFilterConfig,
			std::move(feature_name),
			{false, false, false}
		};
	}

	inline AutomationInvocation MakeExportFilterRunInvocation(std::string feature_name) {
		return {
			AutomationInvocationKind::ExportFilterRun,
			std::move(feature_name),
			{true, false, false}
		};
	}
}
