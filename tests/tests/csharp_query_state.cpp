#include <main.h>

#include "../../src/coreclr/dotnet_query_state.h"

TEST(csharp_query_state, unconditional_requirements_match_empty_host_state) {
	Automation4::DotNetMacroQueryState requirements;
	Automation4::DotNetMacroHostStateSnapshot state;

	EXPECT_TRUE(requirements.IsUnconditional());
	EXPECT_TRUE(Automation4::MatchesDotNetMacroQueryState(requirements, state));
}

TEST(csharp_query_state, minimum_selection_is_checked_without_managed_code) {
	Automation4::DotNetMacroQueryState requirements;
	requirements.minimum_selected_events = 2;
	Automation4::DotNetMacroHostStateSnapshot state;

	EXPECT_FALSE(requirements.IsUnconditional());
	EXPECT_FALSE(Automation4::MatchesDotNetMacroQueryState(requirements, state));
	state.selected_event_count = 1;
	EXPECT_FALSE(Automation4::MatchesDotNetMacroQueryState(requirements, state));
	state.selected_event_count = 2;
	EXPECT_TRUE(Automation4::MatchesDotNetMacroQueryState(requirements, state));
}

TEST(csharp_query_state, all_declared_host_capabilities_must_be_available) {
	Automation4::DotNetMacroQueryState requirements;
	requirements.requires_subtitle_file = true;
	requirements.requires_active_event = true;
	requirements.requires_video = true;
	requirements.requires_audio = true;
	requirements.requires_keyframes = true;

	Automation4::DotNetMacroHostStateSnapshot state;
	EXPECT_FALSE(Automation4::MatchesDotNetMacroQueryState(requirements, state));

	state.has_subtitle_file = true;
	state.has_active_event = true;
	state.has_video = true;
	state.has_audio = true;
	state.has_keyframes = true;
	EXPECT_TRUE(Automation4::MatchesDotNetMacroQueryState(requirements, state));

	state.has_video = false;
	EXPECT_FALSE(Automation4::MatchesDotNetMacroQueryState(requirements, state));
}

TEST(csharp_query_state, invalid_negative_selection_requirement_never_matches) {
	Automation4::DotNetMacroQueryState requirements;
	requirements.minimum_selected_events = -1;
	Automation4::DotNetMacroHostStateSnapshot state;
	state.selected_event_count = 100;

	EXPECT_FALSE(Automation4::MatchesDotNetMacroQueryState(requirements, state));
}
