#include <main.h>

#include "../../src/async_video_provider.h"
#include "../../src/include/aegisub/subtitles_provider.h"
#include "../../src/include/aegisub/video_provider.h"
#include "../../src/video_frame.h"
#include "../../src/video_session_ops.h"

#include <libaegisub/fs.h>
#include <libaegisub/make_unique.h>

namespace {

class FakeVideoProvider final : public VideoProvider {
public:
	double dar = 1.0;
	agi::vfr::Framerate fps = agi::vfr::Framerate(24.0);
	std::vector<int> keyframes = {};
	std::string warning;
	bool has_audio = false;

	void GetFrame(int, VideoFrame&) override { }
	void SetColorSpace(std::string const&) override { }
	int GetFrameCount() const override { return 100; }
	int GetWidth() const override { return 640; }
	int GetHeight() const override { return 480; }
	double GetDAR() const override { return dar; }
	agi::vfr::Framerate GetFPS() const override { return fps; }
	std::vector<int> GetKeyFrames() const override { return keyframes; }
	std::string GetColorSpace() const override { return "BT.709"; }
	std::string GetDecoderName() const override { return "FakeVideoProvider"; }
	std::string GetWarning() const override { return warning; }
	bool HasAudio() const override { return has_audio; }
};

class FakeSubtitlesProvider final : public SubtitlesProvider {
	void LoadSubtitles(char const*, size_t) override { }

public:
	void DrawSubtitles(VideoFrame&, double) override { }
};

}

TEST(video_session_ops, build_opened_video_summary_collects_provider_metadata) {
	auto provider = agi::make_unique<FakeVideoProvider>();
	provider->dar = 1.85;
	provider->fps = agi::vfr::Framerate(24000, 1001);
	provider->keyframes = {5, 10, 20};
	provider->warning = "seek warning";
	provider->has_audio = true;

	AsyncVideoProvider async_provider(
		std::move(provider),
		agi::make_unique<FakeSubtitlesProvider>(),
		[](std::unique_ptr<wxEvent>) { });
	int subtitle_probe_count = 0;
	auto summary = aegisub::video_session_ops::BuildOpenedVideoSummary(
		async_provider,
		agi::fs::PathFromString("movie.mkv"),
		[&](agi::fs::path const& path) {
			++subtitle_probe_count;
			return agi::fs::PathToString(path) == "movie.mkv";
		});

	ASSERT_TRUE(summary.display_aspect_ratio_override.has_value());
	EXPECT_DOUBLE_EQ(1.85, *summary.display_aspect_ratio_override);
	EXPECT_DOUBLE_EQ(24000.0 / 1001.0, summary.timecodes.FPS());
	EXPECT_EQ((std::vector<int>{5, 10, 20}), summary.keyframes);
	EXPECT_EQ("seek warning", summary.warning);
	EXPECT_TRUE(summary.has_audio);
	EXPECT_TRUE(summary.has_subtitles);
	EXPECT_EQ(1, subtitle_probe_count);
}

TEST(video_session_ops, build_opened_video_summary_skips_subtitle_probe_for_non_mkv_and_missing_dar) {
	auto provider = agi::make_unique<FakeVideoProvider>();
	provider->dar = 0.0;

	AsyncVideoProvider async_provider(
		std::move(provider),
		agi::make_unique<FakeSubtitlesProvider>(),
		[](std::unique_ptr<wxEvent>) { });
	int subtitle_probe_count = 0;
	auto summary = aegisub::video_session_ops::BuildOpenedVideoSummary(
		async_provider,
		agi::fs::PathFromString("movie.mp4"),
		[&](agi::fs::path const&) {
			++subtitle_probe_count;
			return true;
		});

	EXPECT_FALSE(summary.display_aspect_ratio_override.has_value());
	EXPECT_FALSE(summary.has_subtitles);
	EXPECT_EQ(0, subtitle_probe_count);
}

TEST(video_session_ops, plan_post_open_uses_aspect_override_and_audio_capability) {
	aegisub::video_session_ops::OpenedVideoSummary summary;
	summary.display_aspect_ratio_override = 1.777777;
	summary.has_audio = true;

	auto plan = aegisub::video_session_ops::PlanPostOpen(
		summary,
		true,
		agi::fs::PathFromString("audio.flac"),
		agi::fs::PathFromString("video.mkv"));

	ASSERT_TRUE(plan.display_aspect_ratio_override.has_value());
	EXPECT_DOUBLE_EQ(1.777777, *plan.display_aspect_ratio_override);
	EXPECT_TRUE(plan.auto_load_linked_audio);
	EXPECT_EQ(0, plan.initial_frame);
}

TEST(video_session_ops, plan_post_open_skips_audio_when_disabled_missing_or_already_bound) {
	aegisub::video_session_ops::OpenedVideoSummary summary;

	auto disabled = aegisub::video_session_ops::PlanPostOpen(
		summary,
		false,
		agi::fs::PathFromString("audio.flac"),
		agi::fs::PathFromString("video.mkv"));
	EXPECT_FALSE(disabled.auto_load_linked_audio);
	EXPECT_FALSE(disabled.display_aspect_ratio_override.has_value());

	summary.has_audio = true;
	auto already_bound = aegisub::video_session_ops::PlanPostOpen(
		summary,
		true,
		agi::fs::PathFromString("video.mkv"),
		agi::fs::PathFromString("video.mkv"));
	EXPECT_FALSE(already_bound.auto_load_linked_audio);
}
