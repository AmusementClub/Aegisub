#include <main.h>

#include "../../src/subtitle_fps_choice.h"

#include <libaegisub/exception.h>

TEST(subtitle_fps_choice, includes_video_choice_for_loaded_cfr_video) {
	auto fps = agi::vfr::Framerate(25, 1);

	auto model = BuildSubtitleFpsChoiceModel(true, false, fps);

	EXPECT_TRUE(model.includes_video_choice);
	EXPECT_FALSE(model.show_smpte);
	EXPECT_EQ(12u, model.choices.size());
}

TEST(subtitle_fps_choice, omits_video_choice_for_vfr_when_not_allowed) {
	auto fps = agi::vfr::Framerate({0, 40, 90});

	auto model = BuildSubtitleFpsChoiceModel(false, true, fps);

	EXPECT_FALSE(model.includes_video_choice);
	EXPECT_TRUE(model.show_smpte);
	EXPECT_EQ(12u, model.choices.size());
}

TEST(subtitle_fps_choice, selecting_video_choice_returns_input_fps) {
	auto fps = agi::vfr::Framerate(24000, 1001);
	auto model = BuildSubtitleFpsChoiceModel(true, true, fps);

	auto selected = ResolveSubtitleFpsChoiceSelection(model, 0, fps);

	EXPECT_TRUE(selected.IsLoaded());
	EXPECT_NEAR(fps.FPS(), selected.FPS(), 1e-9);
}

TEST(subtitle_fps_choice, selecting_dropframe_choice_returns_dropframe_ntsc) {
	auto fps = agi::vfr::Framerate();
	auto model = BuildSubtitleFpsChoiceModel(false, true, fps);

	auto selected = ResolveSubtitleFpsChoiceSelection(model, 5, fps);

	EXPECT_TRUE(selected.IsLoaded());
	EXPECT_TRUE(selected.NeedsDropFrames());
	EXPECT_NEAR(30000.0 / 1001.0, selected.FPS(), 1e-9);
}

TEST(subtitle_fps_choice, selecting_last_non_smpte_choice_returns_120fps) {
	auto fps = agi::vfr::Framerate();
	auto model = BuildSubtitleFpsChoiceModel(false, false, fps);

	auto selected = ResolveSubtitleFpsChoiceSelection(model, 10, fps);

	EXPECT_TRUE(selected.IsLoaded());
	EXPECT_NEAR(120.0, selected.FPS(), 1e-9);
}

TEST(subtitle_fps_choice, rejects_out_of_bounds_choice) {
	auto fps = agi::vfr::Framerate();
	auto model = BuildSubtitleFpsChoiceModel(false, false, fps);

	EXPECT_THROW(ResolveSubtitleFpsChoiceSelection(model, 99, fps), agi::InternalError);
}
