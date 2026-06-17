#include <main.h>

#include "../../src/project_session_ops.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct capture_notification_sink final : agi::NotificationSink {
	std::vector<std::pair<std::string, std::string>> infos;
	std::vector<std::pair<std::string, std::string>> warnings;
	std::vector<std::pair<std::string, std::string>> errors;

	void ShowInfo(std::string const& title, std::string const& message) override {
		infos.emplace_back(title, message);
	}

	void ShowError(std::string const& title, std::string const& message) override {
		errors.emplace_back(title, message);
	}

	void ShowWarning(std::string const& title, std::string const& message) override {
		warnings.emplace_back(title, message);
	}
};

class FakeAudioProvider final : public agi::AudioProvider {
	void FillBuffer(void*, int64_t, int64_t) const override { }

public:
	FakeAudioProvider() {
		channels = 2;
		num_samples = 100;
		sample_rate = 48000;
		bytes_per_sample = 2;
		float_samples = false;
	}
};

}

TEST(project_session_ops, resolve_subtitle_session_target_handles_cancel_current_and_new) {
	using aegisub::project_session_ops::ResolveSubtitleSessionTarget;
	using aegisub::project_session_ops::SubtitleSessionTarget;

	EXPECT_EQ(SubtitleSessionTarget::Cancel, ResolveSubtitleSessionTarget(false, true));
	EXPECT_EQ(SubtitleSessionTarget::CurrentSession, ResolveSubtitleSessionTarget(false, false));
	EXPECT_EQ(SubtitleSessionTarget::NewSession, ResolveSubtitleSessionTarget(true, true));
}

TEST(project_session_ops, execute_subtitle_session_action_runs_only_selected_branch) {
	using aegisub::project_session_ops::ExecuteSubtitleSessionAction;
	using aegisub::project_session_ops::SubtitleSessionTarget;

	int current_count = 0;
	int new_count = 0;

	EXPECT_FALSE(ExecuteSubtitleSessionAction(
		SubtitleSessionTarget::Cancel,
		[&] { ++current_count; },
		[&] { ++new_count; }));
	EXPECT_TRUE(ExecuteSubtitleSessionAction(
		SubtitleSessionTarget::CurrentSession,
		[&] { ++current_count; },
		[&] { ++new_count; }));
	EXPECT_TRUE(ExecuteSubtitleSessionAction(
		SubtitleSessionTarget::NewSession,
		[&] { ++current_count; },
		[&] { ++new_count; }));

	EXPECT_EQ(1, current_count);
	EXPECT_EQ(1, new_count);
}

TEST(project_session_ops, execute_subtitle_load_forwards_path_encoding_and_linked_flag) {
	using aegisub::project_session_ops::ExecuteSubtitleLoad;
	using aegisub::project_session_ops::SubtitleSessionTarget;

	agi::fs::path current_path;
	agi::fs::path new_path;
	std::string current_encoding;
	std::string new_encoding;
	bool current_load_linked = false;
	bool new_load_linked = true;

	ASSERT_TRUE(ExecuteSubtitleLoad(
		SubtitleSessionTarget::CurrentSession,
		agi::fs::path("current.ass"),
		[&](agi::fs::path const& path, std::string const& encoding, bool load_linked) {
			current_path = path;
			current_encoding = encoding;
			current_load_linked = load_linked;
		},
		[&](agi::fs::path const& path, std::string const& encoding, bool load_linked) {
			new_path = path;
			new_encoding = encoding;
			new_load_linked = load_linked;
		},
		"utf-8",
		false));

	ASSERT_TRUE(ExecuteSubtitleLoad(
		SubtitleSessionTarget::NewSession,
		agi::fs::path("new.ass"),
		aegisub::project_session_ops::SubtitleLoadAction{},
		[&](agi::fs::path const& path, std::string const& encoding, bool load_linked) {
			new_path = path;
			new_encoding = encoding;
			new_load_linked = load_linked;
		},
		"shift-jis",
		true));

	EXPECT_EQ(agi::fs::path("current.ass"), current_path);
	EXPECT_EQ("utf-8", current_encoding);
	EXPECT_FALSE(current_load_linked);
	EXPECT_EQ(agi::fs::path("new.ass"), new_path);
	EXPECT_EQ("shift-jis", new_encoding);
	EXPECT_TRUE(new_load_linked);
}

TEST(project_session_ops, resolve_subtitle_encoding_uses_existing_value_or_detector_result) {
	capture_notification_sink sink;
	bool called = false;

	auto existing = aegisub::project_session_ops::ResolveSubtitleEncoding(
		agi::fs::path("subtitles.ass"),
		"utf-8",
		[&]() {
			called = true;
			return std::string("shift-jis");
		},
		sink);

	ASSERT_TRUE(existing.has_value());
	EXPECT_EQ("utf-8", *existing);
	EXPECT_FALSE(called);

	auto detected = aegisub::project_session_ops::ResolveSubtitleEncoding(
		agi::fs::path("subtitles.ass"),
		"",
		[&]() {
			called = true;
			return std::string("shift-jis");
		},
		sink);

	ASSERT_TRUE(detected.has_value());
	EXPECT_EQ("shift-jis", *detected);
	EXPECT_TRUE(called);
	EXPECT_TRUE(sink.errors.empty());
}

TEST(project_session_ops, resolve_subtitle_encoding_handles_cancel_and_missing_files) {
	capture_notification_sink sink;
	std::vector<std::pair<std::string, agi::fs::path>> removed;

	auto cancelled = aegisub::project_session_ops::ResolveSubtitleEncoding(
		agi::fs::path("cancel.ass"),
		"",
		[]() -> std::string {
			throw agi::UserCancelException("cancelled");
		},
		sink,
		[&](char const* category, agi::fs::path const& path) {
			removed.emplace_back(category, path);
		});

	EXPECT_FALSE(cancelled.has_value());
	EXPECT_TRUE(sink.errors.empty());
	EXPECT_TRUE(removed.empty());

	auto missing = aegisub::project_session_ops::ResolveSubtitleEncoding(
		agi::fs::path("missing.ass"),
		"",
		[]() -> std::string {
			throw agi::fs::FileNotFound(agi::fs::path("missing.ass"));
		},
		sink,
		[&](char const* category, agi::fs::path const& path) {
			removed.emplace_back(category, path);
		});

	EXPECT_FALSE(missing.has_value());
	ASSERT_EQ(1u, sink.errors.size());
	EXPECT_EQ("Error loading file", sink.errors[0].first);
	EXPECT_EQ("missing.ass not found.", sink.errors[0].second);
	ASSERT_EQ(1u, removed.size());
	EXPECT_EQ("Subtitle", removed[0].first);
	EXPECT_EQ(agi::fs::path("missing.ass"), removed[0].second);
}

TEST(project_session_ops, load_subtitles_with_error_handling_reports_expected_failures) {
	capture_notification_sink sink;
	std::vector<std::pair<std::string, agi::fs::path>> removed;
	bool loaded = false;

	EXPECT_TRUE(aegisub::project_session_ops::LoadSubtitlesWithErrorHandling(
		agi::fs::path("ok.ass"),
		[&] { loaded = true; },
		sink));
	EXPECT_TRUE(loaded);
	EXPECT_TRUE(sink.errors.empty());

	EXPECT_FALSE(aegisub::project_session_ops::LoadSubtitlesWithErrorHandling(
		agi::fs::path("missing.ass"),
		[] { throw agi::fs::FileNotFound(agi::fs::path("missing.ass")); },
		sink,
		[&](char const* category, agi::fs::path const& path) {
			removed.emplace_back(category, path);
		}));
	EXPECT_FALSE(aegisub::project_session_ops::LoadSubtitlesWithErrorHandling(
		agi::fs::path("broken.ass"),
		[] { throw agi::InternalError("parse failed"); },
		sink));
	EXPECT_FALSE(aegisub::project_session_ops::LoadSubtitlesWithErrorHandling(
		agi::fs::path("runtime.ass"),
		[] { throw std::runtime_error("runtime failed"); },
		sink));
	EXPECT_FALSE(aegisub::project_session_ops::LoadSubtitlesWithErrorHandling(
		agi::fs::path("unknown.ass"),
		[] { throw 42; },
		sink));

	ASSERT_EQ(4u, sink.errors.size());
	EXPECT_EQ("missing.ass not found.", sink.errors[0].second);
	EXPECT_EQ("parse failed", sink.errors[1].second);
	EXPECT_EQ("runtime failed", sink.errors[2].second);
	EXPECT_EQ("Unknown error", sink.errors[3].second);
	ASSERT_EQ(1u, removed.size());
	EXPECT_EQ("Subtitle", removed[0].first);
	EXPECT_EQ(agi::fs::path("missing.ass"), removed[0].second);
}

TEST(project_session_ops, unreadable_audio_open_path_reports_error_and_removes_mru) {
	capture_notification_sink sink;
	std::vector<std::pair<std::string, agi::fs::path>> removed;

	EXPECT_FALSE(aegisub::project_session_ops::HandleUnreadableAudioOpenPath(
		agi::fs::path("broken.wav"),
		"access denied",
		sink,
		[&](char const* category, agi::fs::path const& path) {
			removed.emplace_back(category, path);
		}));

	ASSERT_EQ(1u, sink.errors.size());
	EXPECT_EQ("Error loading file", sink.errors[0].first);
	EXPECT_EQ("The audio file was not found: access denied", sink.errors[0].second);
	ASSERT_EQ(1u, removed.size());
	EXPECT_EQ("Audio", removed[0].first);
	EXPECT_EQ(agi::fs::path("broken.wav"), removed[0].second);
}

TEST(project_session_ops, create_audio_provider_with_error_handling_returns_provider_and_swallows_cancel) {
	capture_notification_sink sink;
	std::vector<std::pair<std::string, agi::fs::path>> removed;

	auto provider = aegisub::project_session_ops::CreateAudioProviderWithErrorHandling(
		agi::fs::path("ok.wav"),
		false,
		[]() -> std::unique_ptr<agi::AudioProvider> {
			return std::make_unique<FakeAudioProvider>();
		},
		sink,
		{},
		[&](char const* category, agi::fs::path const& path) {
			removed.emplace_back(category, path);
		});

	ASSERT_TRUE(provider);
	EXPECT_TRUE(sink.errors.empty());
	EXPECT_TRUE(removed.empty());

	auto cancelled = aegisub::project_session_ops::CreateAudioProviderWithErrorHandling(
		agi::fs::path("cancel.wav"),
		false,
		[]() -> std::unique_ptr<agi::AudioProvider> {
			throw agi::UserCancelException("cancelled");
		},
		sink,
		{},
		[&](char const* category, agi::fs::path const& path) {
			removed.emplace_back(category, path);
		});

	EXPECT_FALSE(cancelled);
	EXPECT_TRUE(sink.errors.empty());
	EXPECT_TRUE(removed.empty());
}

TEST(project_session_ops, open_audio_provider_returns_typed_success_result) {
	aegisub::media_open::MediaOpenRequest request;
	request.kind = aegisub::media_open::MediaKind::Audio;
	request.path = agi::fs::path("ok.wav");

	aegisub::provider_selection_diagnostics::SelectionReport report;
	report.preferred_provider = "FFmpegSource";
	report.selected_provider = "PCM";
	report.attempts = {
		{"FFmpegSource", "not_supported", "not pcm"},
		{"PCM", "opened", ""}
	};

	auto opened = aegisub::project_session_ops::OpenAudioProvider(
		request,
		[]() -> std::unique_ptr<agi::AudioProvider> {
			return std::make_unique<FakeAudioProvider>();
		},
		[&] { return report; });

	ASSERT_TRUE(opened.provider);
	EXPECT_TRUE(opened.result.opened);
	EXPECT_EQ(aegisub::media_open::MediaKind::Audio, opened.result.kind);
	EXPECT_EQ(aegisub::media_open::OpenStatus::Opened, opened.result.status);
	EXPECT_EQ("PCM", opened.result.selected_provider);
	EXPECT_EQ("FFmpegSource", opened.result.provider_report.preferred_provider);
	EXPECT_EQ(2u, opened.result.provider_report.attempts.size());
}

TEST(project_session_ops, open_audio_provider_returns_typed_failures_without_notifications) {
	aegisub::media_open::MediaOpenRequest request;
	request.kind = aegisub::media_open::MediaKind::Audio;
	request.path = agi::fs::path("broken.wav");

	auto no_audio = aegisub::project_session_ops::OpenAudioProvider(
		request,
		[]() -> std::unique_ptr<agi::AudioProvider> {
			throw agi::AudioDataNotFound("No audio found.");
		});
	EXPECT_FALSE(no_audio.provider);
	EXPECT_FALSE(no_audio.result.opened);
	EXPECT_EQ(aegisub::media_open::OpenStatus::NoMedia, no_audio.result.status);
	EXPECT_EQ("No audio found.", no_audio.result.error);

	auto codec_error = aegisub::project_session_ops::OpenAudioProvider(
		request,
		[]() -> std::unique_ptr<agi::AudioProvider> {
			throw agi::AudioProviderError("CodecA");
		});
	EXPECT_FALSE(codec_error.provider);
	EXPECT_EQ(aegisub::media_open::OpenStatus::NotSupported, codec_error.result.status);
	EXPECT_EQ("CodecA", codec_error.result.error);

	auto missing = aegisub::project_session_ops::OpenAudioProvider(
		request,
		[]() -> std::unique_ptr<agi::AudioProvider> {
			throw agi::fs::FileNotFound(agi::fs::path("missing.wav"));
		});
	EXPECT_FALSE(missing.provider);
	EXPECT_EQ(aegisub::media_open::OpenStatus::FileNotFound, missing.result.status);
	EXPECT_FALSE(missing.result.error.empty());
}

TEST(project_session_ops, create_audio_provider_with_error_handling_reports_expected_errors) {
	capture_notification_sink sink;
	std::vector<std::pair<std::string, agi::fs::path>> removed;
	std::string quiet_message;

	auto quiet = aegisub::project_session_ops::CreateAudioProviderWithErrorHandling(
		agi::fs::path("quiet.wav"),
		true,
		[]() -> std::unique_ptr<agi::AudioProvider> {
			throw agi::AudioDataNotFound("No audio found.");
		},
		sink,
		[&](std::string const& message) {
			quiet_message = message;
		},
		[&](char const* category, agi::fs::path const& path) {
			removed.emplace_back(category, path);
		});
	EXPECT_FALSE(quiet);
	EXPECT_EQ("No audio found.", quiet_message);
	EXPECT_TRUE(sink.errors.empty());

	auto loud = aegisub::project_session_ops::CreateAudioProviderWithErrorHandling(
		agi::fs::path("loud.wav"),
		false,
		[]() -> std::unique_ptr<agi::AudioProvider> {
			throw agi::AudioDataNotFound("ProviderA\nProviderB");
		},
		sink,
		{},
		[&](char const* category, agi::fs::path const& path) {
			removed.emplace_back(category, path);
		});
	EXPECT_FALSE(loud);

	auto codec = aegisub::project_session_ops::CreateAudioProviderWithErrorHandling(
		agi::fs::path("codec.wav"),
		false,
		[]() -> std::unique_ptr<agi::AudioProvider> {
			throw agi::AudioProviderError("CodecA");
		},
		sink,
		{},
		[&](char const* category, agi::fs::path const& path) {
			removed.emplace_back(category, path);
		});
	EXPECT_FALSE(codec);

	auto internal = aegisub::project_session_ops::CreateAudioProviderWithErrorHandling(
		agi::fs::path("internal.wav"),
		false,
		[]() -> std::unique_ptr<agi::AudioProvider> {
			throw agi::InternalError("internal failure");
		},
		sink,
		{},
		[&](char const* category, agi::fs::path const& path) {
			removed.emplace_back(category, path);
		});
	EXPECT_FALSE(internal);

	ASSERT_EQ(3u, sink.errors.size());
	EXPECT_EQ("Error loading file", sink.errors[0].first);
	EXPECT_EQ("None of the available audio providers recognised the selected file as containing audio data.\n\nThe following providers were tried:\nProviderA\nProviderB", sink.errors[0].second);
	EXPECT_EQ("None of the available audio providers have a codec available to handle the selected file.\n\nThe following providers were tried:\nCodecA", sink.errors[1].second);
	EXPECT_EQ("internal failure", sink.errors[2].second);
	ASSERT_EQ(4u, removed.size());
	EXPECT_EQ("Audio", removed[0].first);
	EXPECT_EQ(agi::fs::path("quiet.wav"), removed[0].second);
	EXPECT_EQ(agi::fs::path("loud.wav"), removed[1].second);
	EXPECT_EQ(agi::fs::path("codec.wav"), removed[2].second);
	EXPECT_EQ(agi::fs::path("internal.wav"), removed[3].second);
}

TEST(project_session_ops, save_timecodes_to_path_records_mru_after_success) {
	capture_notification_sink sink;
	std::vector<std::pair<std::string, agi::fs::path>> mru_entries;
	int frame_count = -1;

	ASSERT_TRUE(aegisub::project_session_ops::SaveTimecodesToPath(
		agi::fs::path("timecodes.txt"),
		321,
		[&](agi::fs::path const& path, int frames) {
			EXPECT_EQ(agi::fs::path("timecodes.txt"), path);
			frame_count = frames;
		},
		sink,
		[&](char const* category, agi::fs::path const& path) {
			mru_entries.emplace_back(category, path);
		}));

	EXPECT_EQ(321, frame_count);
	ASSERT_EQ(1u, mru_entries.size());
	EXPECT_EQ("Timecodes", mru_entries[0].first);
	EXPECT_EQ(agi::fs::path("timecodes.txt"), mru_entries[0].second);
	EXPECT_TRUE(sink.errors.empty());
}

TEST(project_session_ops, save_timecodes_to_path_reports_agi_errors_and_skips_mru) {
	capture_notification_sink sink;
	bool added_mru = false;

	EXPECT_FALSE(aegisub::project_session_ops::SaveTimecodesToPath(
		agi::fs::path("timecodes.txt"),
		10,
		[](agi::fs::path const&, int) {
			throw agi::InternalError("broken timecodes");
		},
		sink,
		[&](char const*, agi::fs::path const&) {
			added_mru = true;
		}));

	EXPECT_FALSE(added_mru);
	ASSERT_EQ(1u, sink.errors.size());
	EXPECT_EQ("Error saving timecodes", sink.errors[0].first);
	EXPECT_EQ("broken timecodes", sink.errors[0].second);
}

TEST(project_session_ops, save_keyframes_to_path_reports_std_errors_and_skips_mru) {
	capture_notification_sink sink;
	bool added_mru = false;

	EXPECT_FALSE(aegisub::project_session_ops::SaveKeyframesToPath(
		agi::fs::path("keyframes.txt"),
		[](agi::fs::path const&) {
			throw std::runtime_error("save failed");
		},
		sink,
		[&](char const*, agi::fs::path const&) {
			added_mru = true;
		}));

	EXPECT_FALSE(added_mru);
	ASSERT_EQ(1u, sink.errors.size());
	EXPECT_EQ("Error saving keyframes", sink.errors[0].first);
	EXPECT_EQ("save failed", sink.errors[0].second);
}
