#include <main.h>

#include "../../src/project_session_ops.h"

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
