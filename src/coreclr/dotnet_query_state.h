#pragma once

#include <cstddef>

namespace Automation4 {

struct DotNetMacroQueryState {
	bool requires_subtitle_file = false;
	int minimum_selected_events = 0;
	bool requires_active_event = false;
	bool requires_video = false;
	bool requires_audio = false;
	bool requires_keyframes = false;

	bool IsUnconditional() const noexcept;
	bool operator==(DotNetMacroQueryState const&) const = default;
};

struct DotNetMacroHostStateSnapshot {
	bool has_subtitle_file = false;
	std::size_t selected_event_count = 0;
	bool has_active_event = false;
	bool has_video = false;
	bool has_audio = false;
	bool has_keyframes = false;
};

bool MatchesDotNetMacroQueryState(
	DotNetMacroQueryState const& requirements,
	DotNetMacroHostStateSnapshot const& state) noexcept;

} // namespace Automation4
