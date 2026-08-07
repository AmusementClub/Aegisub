#include <main.h>

#include "../../src/secondary_subtitle_reload_policy.h"

TEST(secondary_subtitle_reload_policy, inactive_session_releases_provider) {
	EXPECT_EQ(
		SecondarySubtitleExternalReloadAction::ReleaseProvider,
		PlanSecondarySubtitleExternalReload(false, true, true));
}

TEST(secondary_subtitle_reload_policy, active_srt_with_provider_reloads_in_place) {
	EXPECT_EQ(
		SecondarySubtitleExternalReloadAction::ReloadInPlace,
		PlanSecondarySubtitleExternalReload(true, true, true));
}

TEST(secondary_subtitle_reload_policy, active_srt_without_provider_rebuilds) {
	EXPECT_EQ(
		SecondarySubtitleExternalReloadAction::RebuildProvider,
		PlanSecondarySubtitleExternalReload(true, false, true));
}

TEST(secondary_subtitle_reload_policy, formats_without_in_place_support_rebuild) {
	EXPECT_EQ(
		SecondarySubtitleExternalReloadAction::RebuildProvider,
		PlanSecondarySubtitleExternalReload(true, true, false));
}
