#include <main.h>

#include "../../src/video_subtitle_scene_cache.h"
#include "../../src/video_subtitle_update_policy.h"

#include <libaegisub/vfr.h>

#include <string>
#include <vector>

namespace {
AssDialogueBase MakeLine(
	int start_ms,
	int end_ms,
	std::string text,
	std::string effect = {},
	bool comment = false) {
	AssDialogueBase line;
	line.Start = start_ms;
	line.End = end_ms;
	line.Text = std::move(text);
	line.Effect = std::move(effect);
	line.Comment = comment;
	return line;
}
}

TEST(video_subtitle_scene_cache, visible_text_commit_waits) {
	auto const fps = agi::vfr::Framerate(100.0);
	std::vector<AssDialogueBase> lines = {
		MakeLine(1000, 2000, "old"),
	};
	auto const snapshot = video_subtitle_scene_cache::CaptureSubtitleSceneSnapshot(lines, fps, 1500);

	lines[0].Text = "new";
	bool const wait = video_subtitle_scene_cache::ShouldWaitForFreshPacket(
		AssFile::COMMIT_DIAG_TEXT,
		true,
		true,
		lines,
		fps,
		1500,
		snapshot);
	EXPECT_TRUE(wait);
}

TEST(video_subtitle_scene_cache, invisible_text_commit_skips_wait) {
	auto const fps = agi::vfr::Framerate(100.0);
	std::vector<AssDialogueBase> lines = {
		MakeLine(1000, 2000, "visible"),
		MakeLine(3000, 4000, "hidden"),
	};
	auto const snapshot = video_subtitle_scene_cache::CaptureSubtitleSceneSnapshot(lines, fps, 1500);

	lines[1].Text = "hidden changed";
	bool const wait = video_subtitle_scene_cache::ShouldWaitForFreshPacket(
		AssFile::COMMIT_DIAG_TEXT,
		true,
		true,
		lines,
		fps,
		1500,
		snapshot);
	EXPECT_FALSE(wait);
}

TEST(video_subtitle_scene_cache, multiline_commit_uses_snapshot_diff_when_single_line_missing) {
	auto const fps = agi::vfr::Framerate(100.0);
	std::vector<AssDialogueBase> lines = {
		MakeLine(1000, 2000, "visible"),
		MakeLine(3000, 4000, "hidden"),
	};
	auto const snapshot = video_subtitle_scene_cache::CaptureSubtitleSceneSnapshot(lines, fps, 1500);

	{
		auto changed = lines;
		changed[1].Text = "hidden changed";
		bool const wait = video_subtitle_scene_cache::ShouldWaitForFreshPacket(
			AssFile::COMMIT_DIAG_TEXT,
			true,
			false,
			changed,
			fps,
			1500,
			snapshot);
		EXPECT_FALSE(wait);
	}

	{
		auto changed = lines;
		changed[0].Text = "visible changed";
		bool const wait = video_subtitle_scene_cache::ShouldWaitForFreshPacket(
			AssFile::COMMIT_DIAG_TEXT,
			true,
			false,
			changed,
			fps,
			1500,
			snapshot);
		EXPECT_TRUE(wait);
	}
}

TEST(video_subtitle_scene_cache, style_commit_waits_only_when_frame_has_visible_lines) {
	auto const fps = agi::vfr::Framerate(100.0);

	{
		std::vector<AssDialogueBase> lines = {
			MakeLine(1000, 2000, "visible"),
		};
		auto const snapshot = video_subtitle_scene_cache::CaptureSubtitleSceneSnapshot(lines, fps, 1500);
		bool const wait = video_subtitle_scene_cache::ShouldWaitForFreshPacket(
			AssFile::COMMIT_STYLES,
			true,
			false,
			lines,
			fps,
			1500,
			snapshot);
		EXPECT_TRUE(wait);
	}

	{
		std::vector<AssDialogueBase> lines = {
			MakeLine(3000, 4000, "hidden"),
		};
		auto const snapshot = video_subtitle_scene_cache::CaptureSubtitleSceneSnapshot(lines, fps, 1500);
		EXPECT_TRUE(snapshot.empty());
		bool const wait = video_subtitle_scene_cache::ShouldWaitForFreshPacket(
			AssFile::COMMIT_STYLES,
			true,
			false,
			lines,
			fps,
			1500,
			snapshot);
		EXPECT_FALSE(wait);
	}
}

TEST(video_subtitle_scene_cache, static_timing_change_skips_wait_even_without_single_changed_line) {
	auto const fps = agi::vfr::Framerate(100.0);
	std::vector<AssDialogueBase> lines = {
		MakeLine(1000, 2000, "plain text"),
	};
	auto const snapshot = video_subtitle_scene_cache::CaptureSubtitleSceneSnapshot(lines, fps, 1500);

	lines[0].Start = 1100;
	lines[0].End = 2100;
	bool const wait = video_subtitle_scene_cache::ShouldWaitForFreshPacket(
		AssFile::COMMIT_DIAG_TIME,
		true,
		false,
		lines,
		fps,
		1500,
		snapshot);
	EXPECT_FALSE(wait);
}

TEST(video_subtitle_scene_cache, animated_timing_change_waits_even_without_single_changed_line) {
	auto const fps = agi::vfr::Framerate(100.0);
	std::vector<AssDialogueBase> lines = {
		MakeLine(1000, 2000, "{\\move(0,0,100,100)}animated"),
	};
	auto const snapshot = video_subtitle_scene_cache::CaptureSubtitleSceneSnapshot(lines, fps, 1500);

	lines[0].Start = 1100;
	lines[0].End = 2100;
	bool const wait = video_subtitle_scene_cache::ShouldWaitForFreshPacket(
		AssFile::COMMIT_DIAG_TIME,
		true,
		false,
		lines,
		fps,
		1500,
		snapshot);
	EXPECT_TRUE(wait);
}

TEST(video_subtitle_update_policy, pure_dialogue_commits_use_single_line_updates) {
	AssDialogue changed;
	AssDialogue const *changed_lines[] = {&changed};
	auto const incremental = video_subtitle_update_policy::UpdateMode::IncrementalLines;

	EXPECT_EQ(incremental, video_subtitle_update_policy::SelectUpdateMode(AssFile::COMMIT_DIAG_META, changed_lines));
	EXPECT_EQ(incremental, video_subtitle_update_policy::SelectUpdateMode(AssFile::COMMIT_DIAG_TIME, changed_lines));
	EXPECT_EQ(incremental, video_subtitle_update_policy::SelectUpdateMode(AssFile::COMMIT_DIAG_TEXT, changed_lines));
	EXPECT_EQ(incremental, video_subtitle_update_policy::SelectUpdateMode(AssFile::COMMIT_DIAG_FULL, changed_lines));
}

TEST(video_subtitle_update_policy, missing_changed_line_requires_full_reload) {
	auto const full_reload = video_subtitle_update_policy::UpdateMode::FullReload;
	EXPECT_EQ(full_reload, video_subtitle_update_policy::SelectUpdateMode(AssFile::COMMIT_DIAG_TEXT, {}));
}

TEST(video_subtitle_update_policy, project_session_insert_with_single_line_requires_full_reload) {
	AssDialogue inserted;
	AssDialogue const *changed_lines[] = {&inserted};
	auto const full_reload = video_subtitle_update_policy::UpdateMode::FullReload;
	EXPECT_EQ(full_reload, video_subtitle_update_policy::SelectUpdateMode(AssFile::COMMIT_DIAG_ADDREM, changed_lines));
}

TEST(video_subtitle_update_policy, non_dialogue_and_mixed_commits_require_full_reload) {
	AssDialogue changed;
	AssDialogue const *changed_lines[] = {&changed};
	auto const full_reload = video_subtitle_update_policy::UpdateMode::FullReload;
	int const full_reload_types[] = {
		AssFile::COMMIT_NEW,
		AssFile::COMMIT_ORDER,
		AssFile::COMMIT_SCRIPTINFO,
		AssFile::COMMIT_STYLES,
		AssFile::COMMIT_ATTACHMENT,
		AssFile::COMMIT_DIAG_ADDREM,
		AssFile::COMMIT_EXTRADATA,
		AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_ORDER,
		AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_SCRIPTINFO,
		AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_STYLES,
		AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_ATTACHMENT,
		AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_DIAG_ADDREM,
		AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_EXTRADATA,
	};

	for (int type : full_reload_types)
		EXPECT_EQ(full_reload, video_subtitle_update_policy::SelectUpdateMode(type, changed_lines)) << type;
}

