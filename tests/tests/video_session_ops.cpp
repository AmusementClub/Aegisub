#include <main.h>

#include "../../src/async_video_provider.h"
#include "../../src/include/aegisub/subtitles_provider.h"
#include "../../src/include/aegisub/video_provider.h"
#include "../../src/ui_services.h"
#include "../../src/video_frame.h"
#include "../../src/video_session_ops.h"

#include <libaegisub/fs.h>
#include <libaegisub/make_unique.h>

namespace {

struct capture_notification_sink final : agi::NotificationSink {
	std::vector<std::pair<std::string, std::string>> errors;

	void ShowInfo(std::string const&, std::string const&) override { }
	void ShowError(std::string const& title, std::string const& message) override {
		errors.emplace_back(title, message);
	}
	void ShowWarning(std::string const&, std::string const&) override { }
};

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
		AsyncVideoProviderEventSink{});
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

TEST(video_session_ops, build_opened_video_summary_from_metadata_is_async_free) {
	aegisub::video_session_ops::OpenedVideoMetadata metadata;
	metadata.display_aspect_ratio_override = 2.35;
	metadata.timecodes = agi::vfr::Framerate(25.0);
	metadata.keyframes = {1, 25};
	metadata.warning = "metadata warning";
	metadata.has_audio = true;

	int subtitle_probe_count = 0;
	auto summary = aegisub::video_session_ops::BuildOpenedVideoSummary(
		metadata,
		agi::fs::PathFromString("movie.mkv"),
		[&](agi::fs::path const&) {
			++subtitle_probe_count;
			return false;
		});

	ASSERT_TRUE(summary.display_aspect_ratio_override.has_value());
	EXPECT_DOUBLE_EQ(2.35, *summary.display_aspect_ratio_override);
	EXPECT_DOUBLE_EQ(25.0, summary.timecodes.FPS());
	EXPECT_EQ((std::vector<int>{1, 25}), summary.keyframes);
	EXPECT_EQ("metadata warning", summary.warning);
	EXPECT_TRUE(summary.has_audio);
	EXPECT_FALSE(summary.has_subtitles);
	EXPECT_EQ(1, subtitle_probe_count);
}

TEST(video_session_ops, build_opened_video_summary_skips_subtitle_probe_for_non_mkv_and_missing_dar) {
	auto provider = agi::make_unique<FakeVideoProvider>();
	provider->dar = 0.0;

	AsyncVideoProvider async_provider(
		std::move(provider),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});
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

TEST(video_session_ops, unreadable_video_open_path_reports_error_and_removes_mru) {
	capture_notification_sink sink;
	std::vector<std::pair<std::string, agi::fs::path>> removed;

	EXPECT_FALSE(aegisub::video_session_ops::HandleUnreadableVideoOpenPath(
		agi::fs::PathFromString("missing.mkv"),
		"not readable",
		sink,
		[&](char const* category, agi::fs::path const& path) {
			removed.emplace_back(category, path);
		}));

	ASSERT_EQ(1u, sink.errors.size());
	EXPECT_EQ("Error loading file", sink.errors[0].first);
	EXPECT_EQ("not readable", sink.errors[0].second);
	ASSERT_EQ(1u, removed.size());
	EXPECT_EQ("Video", removed[0].first);
	EXPECT_EQ(agi::fs::PathFromString("missing.mkv"), removed[0].second);
}

TEST(video_session_ops, create_video_provider_with_error_handling_returns_provider_without_notifications) {
	capture_notification_sink sink;

	auto provider = aegisub::video_session_ops::CreateVideoProviderWithErrorHandling(
		agi::fs::PathFromString("ok.mkv"),
		[] {
			return agi::make_unique<AsyncVideoProvider>(
				agi::make_unique<FakeVideoProvider>(),
				agi::make_unique<FakeSubtitlesProvider>(),
				AsyncVideoProviderEventSink{});
		},
		sink);

	ASSERT_TRUE(provider);
	EXPECT_TRUE(sink.errors.empty());
}

TEST(video_session_ops, open_video_provider_returns_typed_success_result) {
	aegisub::media_open::MediaOpenRequest request;
	request.kind = aegisub::media_open::MediaKind::Video;
	request.path = agi::fs::PathFromString("ok.mkv");

	aegisub::provider_selection_diagnostics::SelectionReport report;
	report.preferred_provider = "FFmpegSource";
	report.selected_provider = "YUV4MPEG";
	report.attempts = {
		{"FFmpegSource", "not_supported", "not y4m"},
		{"YUV4MPEG", "opened", ""}
	};

	auto opened = aegisub::video_session_ops::OpenVideoProvider(
		request,
		[] {
			return agi::make_unique<AsyncVideoProvider>(
				agi::make_unique<FakeVideoProvider>(),
				agi::make_unique<FakeSubtitlesProvider>(),
				AsyncVideoProviderEventSink{});
		},
		[&] { return report; });

	ASSERT_TRUE(opened.provider);
	EXPECT_TRUE(opened.result.opened);
	EXPECT_EQ(aegisub::media_open::OpenStatus::Opened, opened.result.status);
	EXPECT_EQ("YUV4MPEG", opened.result.selected_provider);
	EXPECT_EQ("FakeVideoProvider", opened.result.decoder_name);
	EXPECT_EQ("FFmpegSource", opened.result.provider_report.preferred_provider);
	EXPECT_EQ(2u, opened.result.provider_report.attempts.size());
}

TEST(video_session_ops, open_video_provider_returns_typed_failure_without_notifications) {
	aegisub::media_open::MediaOpenRequest request;
	request.kind = aegisub::media_open::MediaKind::Video;
	request.path = agi::fs::PathFromString("broken.mkv");

	auto provider_error = aegisub::video_session_ops::OpenVideoProvider(
		request,
		[]() -> std::unique_ptr<AsyncVideoProvider> {
			throw VideoOpenError("decoder failed");
		});

	EXPECT_FALSE(provider_error.provider);
	EXPECT_FALSE(provider_error.result.opened);
	EXPECT_EQ(aegisub::media_open::OpenStatus::Error, provider_error.result.status);
	EXPECT_EQ("decoder failed", provider_error.result.error);

	auto fs_error = aegisub::video_session_ops::OpenVideoProvider(
		request,
		[]() -> std::unique_ptr<AsyncVideoProvider> {
			throw agi::fs::FileNotFound(agi::fs::PathFromString("missing.mkv"));
		});

	EXPECT_FALSE(fs_error.provider);
	EXPECT_EQ(aegisub::media_open::OpenStatus::FileNotFound, fs_error.result.status);
	EXPECT_FALSE(fs_error.result.error.empty());
}

TEST(video_session_ops, create_video_provider_with_error_handling_swallows_cancel_without_error) {
	capture_notification_sink sink;

	auto provider = aegisub::video_session_ops::CreateVideoProviderWithErrorHandling(
		agi::fs::PathFromString("cancel.mkv"),
		[]() -> std::unique_ptr<AsyncVideoProvider> {
			throw agi::UserCancelException("cancelled");
		},
		sink);

	EXPECT_FALSE(provider);
	EXPECT_TRUE(sink.errors.empty());
}

TEST(video_session_ops, create_video_provider_with_error_handling_reports_provider_and_fs_errors) {
	capture_notification_sink sink;
	std::vector<std::pair<std::string, agi::fs::path>> removed;

	auto provider_error = aegisub::video_session_ops::CreateVideoProviderWithErrorHandling(
		agi::fs::PathFromString("broken.mkv"),
		[]() -> std::unique_ptr<AsyncVideoProvider> {
			throw VideoOpenError("decoder failed");
		},
		sink,
		[&](char const* category, agi::fs::path const& path) {
			removed.emplace_back(category, path);
		});

	EXPECT_FALSE(provider_error);
	ASSERT_EQ(1u, sink.errors.size());
	EXPECT_EQ("decoder failed", sink.errors[0].second);
	EXPECT_TRUE(removed.empty());

	auto fs_error = aegisub::video_session_ops::CreateVideoProviderWithErrorHandling(
		agi::fs::PathFromString("missing.mkv"),
		[]() -> std::unique_ptr<AsyncVideoProvider> {
			throw agi::fs::FileNotFound(agi::fs::PathFromString("missing.mkv"));
		},
		sink,
		[&](char const* category, agi::fs::path const& path) {
			removed.emplace_back(category, path);
		});

	EXPECT_FALSE(fs_error);
	ASSERT_EQ(2u, sink.errors.size());
	EXPECT_EQ("Error loading file", sink.errors[1].first);
	ASSERT_EQ(1u, removed.size());
	EXPECT_EQ("Video", removed[0].first);
	EXPECT_EQ(agi::fs::PathFromString("missing.mkv"), removed[0].second);
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
