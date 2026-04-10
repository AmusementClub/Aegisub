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

#include <string>
#include <vector>

namespace Automation4 {
	struct AutomationDebugBreakpoint {
		std::string source_path;
		int line = 0;
		bool enabled = true;
	};

	std::string NormalizeAutomationDebugSource(std::string const& source);

	class AutomationBreakpointStore final {
		std::vector<AutomationDebugBreakpoint> breakpoints;

	public:
		void SetBreakpoints(std::vector<AutomationDebugBreakpoint> values);
		std::vector<AutomationDebugBreakpoint> GetBreakpoints() const;
		size_t Count() const;
		bool Matches(std::string const& source_path, int line) const;
	};
}
