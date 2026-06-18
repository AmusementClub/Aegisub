// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
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
//
// Aegisub Project http://www.aegisub.org/

#pragma once

#include <libaegisub/mru.h>
#include <libaegisub/option.h>
#include <libaegisub/option_value.h>

#include <string>
#include <utility>

namespace agi { class Path; }
namespace Automation4 { class AutoloadScriptManager; }
namespace Automation4 { class AutomationDebugService; }

/// For holding all configuration-related objects and values.
namespace config {
	extern agi::Options *opt;    ///< Options
	extern agi::MRUManager *mru; ///< Most Recently Used
	extern agi::Path *path;
	extern Automation4::AutoloadScriptManager *global_scripts;
	extern Automation4::AutomationDebugService *automation_debug_service;

	inline bool GetBoolOptionOrDefault(char const *name, bool default_value) {
		if (!opt)
			return default_value;
		try {
			return opt->Get(name)->GetBool();
		}
		catch (...) {
			return default_value;
		}
	}

	inline int GetIntOptionOrDefault(char const *name, int default_value) {
		if (!opt)
			return default_value;
		try {
			return opt->Get(name)->GetInt();
		}
		catch (...) {
			return default_value;
		}
	}

	inline bool GetBoolOptionOrDefault(std::string const& name, bool default_value) {
		return GetBoolOptionOrDefault(name.c_str(), default_value);
	}

	inline int GetIntOptionOrDefault(std::string const& name, int default_value) {
		return GetIntOptionOrDefault(name.c_str(), default_value);
	}

	inline std::string GetStringOptionOrDefault(char const *name, std::string default_value) {
		if (!opt)
			return default_value;
		try {
			return opt->Get(name)->GetString();
		}
		catch (...) {
			return default_value;
		}
	}

	inline std::string GetStringOptionOrDefault(std::string const& name, std::string default_value) {
		return GetStringOptionOrDefault(name.c_str(), std::move(default_value));
	}
}

/// Macro to get OptionValue object
#define OPT_GET(x) const_cast<const agi::OptionValue*>(config::opt->Get(x))

/// Macro to set OptionValue object
#define OPT_SET(x) config::opt->Get(x)

/// Macro to subscribe to OptionValue changes
#define OPT_SUB(x, ...) config::opt->Get(x)->Subscribe(__VA_ARGS__)
