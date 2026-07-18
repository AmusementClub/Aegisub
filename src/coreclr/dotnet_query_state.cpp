#include "dotnet_query_state.h"

namespace Automation4 {

bool DotNetMacroQueryState::IsUnconditional() const noexcept {
	return !requires_subtitle_file && minimum_selected_events == 0 &&
		!requires_active_event && !requires_video && !requires_audio &&
		!requires_keyframes;
}

bool MatchesDotNetMacroQueryState(
	DotNetMacroQueryState const& requirements,
	DotNetMacroHostStateSnapshot const& state) noexcept {
	if (requirements.minimum_selected_events < 0)
		return false;
	if (requirements.requires_subtitle_file && !state.has_subtitle_file)
		return false;
	if (state.selected_event_count <
		static_cast<std::size_t>(requirements.minimum_selected_events))
		return false;
	if (requirements.requires_active_event && !state.has_active_event)
		return false;
	if (requirements.requires_video && !state.has_video)
		return false;
	if (requirements.requires_audio && !state.has_audio)
		return false;
	if (requirements.requires_keyframes && !state.has_keyframes)
		return false;
	return true;
}

} // namespace Automation4
