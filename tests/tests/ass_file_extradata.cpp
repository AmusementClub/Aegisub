#include <gtest/gtest.h>

#include "../../src/ass_compat.h"
#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_info.h"
#include "../../src/ass_io_core.h"
#include "../../src/ass_style.h"
#include "../../src/subs_controller.h"
#include "../../src/subtitle_format.h"

#include <libaegisub/vfr.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>

TEST(ass_file_commit, detailed_metadata_normalizes_legacy_single_and_explicit_multi_line_changes) {
	AssFile file;
	file.Events.push_back(*new AssDialogue);
	file.Events.push_back(*new AssDialogue);
	auto first = file.Events.begin();
	auto second = std::next(first);
	first->Row = 0;
	second->Row = 1;

	struct Observation {
		AssDialogue *single_line = nullptr;
		std::vector<AssDialogue const *> changed_lines;
	};
	std::vector<Observation> undo_observations;
	std::vector<Observation> detailed_observations;
	std::vector<AssDialogue const *> legacy_observations;

	auto undo_connection = agi::signal::Connection(file.AddUndoManager([&](AssFileCommit commit) {
		undo_observations.push_back({commit.single_line, {commit.changed_lines.begin(), commit.changed_lines.end()}});
	}));
	auto detailed_connection = agi::signal::Connection(file.AddCommitDetailsListener([&](AssFileCommitDetails commit) {
		detailed_observations.push_back({commit.single_line, {commit.changed_lines.begin(), commit.changed_lines.end()}});
	}));
	auto legacy_connection = agi::signal::Connection(file.AddCommitListener([&](int, AssDialogue const *single_line) {
		legacy_observations.push_back(single_line);
	}));

	file.Commit("legacy single", AssFile::COMMIT_DIAG_TEXT, -1, &*first);
	std::vector<AssDialogue const *> multiple = {&*first, &*second};
	file.Commit("explicit multi", AssFile::COMMIT_DIAG_TEXT, -1, &*first, multiple);
	file.Commit("explicit empty", AssFile::COMMIT_DIAG_TEXT, -1, &*first, {});

	ASSERT_EQ(3u, undo_observations.size());
	ASSERT_EQ(3u, detailed_observations.size());
	ASSERT_EQ(3u, legacy_observations.size());

	EXPECT_EQ(&*first, undo_observations[0].single_line);
	ASSERT_EQ(1u, undo_observations[0].changed_lines.size());
	EXPECT_EQ(&*first, undo_observations[0].changed_lines[0]);
	EXPECT_EQ(&*first, detailed_observations[0].single_line);
	EXPECT_EQ(&*first, legacy_observations[0]);

	EXPECT_EQ(nullptr, undo_observations[1].single_line);
	EXPECT_EQ(multiple, undo_observations[1].changed_lines);
	EXPECT_EQ(nullptr, detailed_observations[1].single_line);
	EXPECT_EQ(multiple, detailed_observations[1].changed_lines);
	EXPECT_EQ(nullptr, legacy_observations[1]);

	EXPECT_EQ(nullptr, undo_observations[2].single_line);
	EXPECT_TRUE(undo_observations[2].changed_lines.empty());
	EXPECT_EQ(nullptr, detailed_observations[2].single_line);
	EXPECT_TRUE(detailed_observations[2].changed_lines.empty());
	EXPECT_EQ(nullptr, legacy_observations[2]);
}

TEST(subs_controller_undo, amends_changed_dialogue_rows_by_row_and_id) {
	AssDialogue first;
	first.Row = 0;
	first.Text = "first before";
	AssDialogue second;
	second.Row = 1;
	second.Text = "second before";
	std::vector<AssDialogueBase> snapshot = {first, second};

	first.Text = "first after";
	second.Text = "second after";
	std::vector<AssDialogue const *> changed_lines = {&second, &first};

	ASSERT_TRUE(subs_controller_detail::TryAmendDialogueSnapshot(snapshot, changed_lines));
	EXPECT_EQ("first after", snapshot[0].Text.get());
	EXPECT_EQ("second after", snapshot[1].Text.get());
}

TEST(subs_controller_undo, rejects_invalid_rows_without_partial_update) {
	AssDialogue first;
	first.Row = 0;
	first.Text = "first before";
	AssDialogue second;
	second.Row = 1;
	second.Text = "second before";
	std::vector<AssDialogueBase> snapshot = {first, second};

	first.Text = "first after";
	AssDialogue wrong_line;
	wrong_line.Row = 1;
	wrong_line.Text = "wrong";
	std::vector<AssDialogue const *> invalid_lines = {&first, &wrong_line};
	EXPECT_FALSE(subs_controller_detail::TryAmendDialogueSnapshot(snapshot, invalid_lines));
	EXPECT_EQ("first before", snapshot[0].Text.get());
	EXPECT_EQ("second before", snapshot[1].Text.get());
}

TEST(subs_controller_save_on_change, retains_only_the_latest_pending_snapshot) {
	subs_controller_detail::LatestSaveState<int> state;
	auto const generation = state.BeginRevision();

	ASSERT_TRUE(state.Submit(generation, 10));
	ASSERT_TRUE(state.Submit(generation, 20));
	auto request = state.TakePending();

	ASSERT_TRUE(request.has_value());
	EXPECT_EQ(20, *request);
	EXPECT_FALSE(state.TakePending().has_value());
}

TEST(subs_controller_save_on_change, newer_revision_prevents_stale_publish) {
	subs_controller_detail::LatestSaveState<int> state;
	auto const old_generation = state.BeginRevision();
	ASSERT_TRUE(state.Submit(old_generation, 10));
	auto active = state.TakePending();
	ASSERT_TRUE(active.has_value());

	auto const new_generation = state.BeginRevision();
	EXPECT_FALSE(state.CanPublish(old_generation));
	EXPECT_TRUE(state.Submit(new_generation, 20));
	EXPECT_TRUE(state.CanPublish(new_generation));
}

TEST(subs_controller_save_on_change, stop_discards_pending_and_rejects_new_work) {
	subs_controller_detail::LatestSaveState<int> state;
	auto const generation = state.BeginRevision();
	ASSERT_TRUE(state.Submit(generation, 10));

	state.Stop();

	EXPECT_TRUE(state.IsStopping());
	EXPECT_FALSE(state.TakePending().has_value());
	EXPECT_FALSE(state.CanPublish(generation));
	EXPECT_FALSE(state.Submit(state.Generation(), 20));
}

TEST(subs_controller_save_on_change, subtitle_formats_must_explicitly_opt_in_to_background_writes) {
	class UnspecifiedFormat final : public SubtitleFormat {
	public:
		UnspecifiedFormat() : SubtitleFormat("test") { }
	};

	UnspecifiedFormat format;
	EXPECT_FALSE(format.SupportsBackgroundWriting());
}

TEST(ass_file_extradata, cleaning_copy_does_not_mutate_original_file_state) {
	AssFile original;

	auto duplicate_a = original.AddExtradata("Key", "Value A");
	auto duplicate_b = original.AddExtradata("Key", "Value B");
	original.AddExtradata("Unused", "Orphaned");

	auto line = new AssDialogue;
	line->ExtradataIds = std::vector<uint32_t>{duplicate_a, duplicate_b};
	original.Events.push_back(*line);

	AssFile copy(original);
	copy.CleanExtradata();

	ASSERT_EQ(3u, original.Extradata.size());
	ASSERT_EQ(2u, original.Events.front().ExtradataIds.get().size());

	EXPECT_EQ(1u, copy.Extradata.size());
	EXPECT_EQ(1u, copy.Events.front().ExtradataIds.get().size());
}

TEST(ass_file_style_lookup, matches_renderer_compatible_default_and_starred_names) {
	AssFile file;
	file.Styles.push_back(*new AssStyle("Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1"));
	file.Styles.push_back(*new AssStyle("Style: *Starred,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1"));

	EXPECT_EQ("Default", file.GetStyle("*default")->name);
	EXPECT_EQ("*Starred", file.GetStyle("Starred")->name);
	EXPECT_EQ("*Starred", file.GetStyle("**Starred")->name);
}

TEST(ass_file_style_lookup, keeps_non_default_style_names_case_sensitive) {
	AssFile file;
	file.Styles.push_back(*new AssStyle("Style: Foo,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1"));
	file.Styles.push_back(*new AssStyle("Style: foo,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1"));

	EXPECT_EQ("Foo", file.GetStyle("Foo")->name);
	EXPECT_EQ("foo", file.GetStyle("foo")->name);
	EXPECT_EQ(nullptr, file.GetStyle("FOO"));
}

TEST(ass_file_style_lookup, shared_style_map_lookup_matches_renderer_compatible_names) {
	std::map<std::string, int> styles;
	styles["Default"] = 1;
	styles["*Starred"] = 2;
	styles["Foo"] = 3;

	EXPECT_EQ(1, AssCompat::FindStyle(styles, "*default")->second);
	EXPECT_EQ(2, AssCompat::FindStyle(styles, "Starred")->second);
	EXPECT_EQ(styles.end(), AssCompat::FindStyle(styles, "foo"));
}

TEST(ass_file_style_lookup, analyzes_renderer_compatibility_repairs_without_rewriting_on_read) {
	auto starred = AssCompat::AnalyzeStyleName("*Starred");
	EXPECT_TRUE(starred.has_compatibility_prefix);
	EXPECT_FALSE(starred.has_default_case_mismatch);
	EXPECT_TRUE(starred.NeedsExplicitRepair());
	EXPECT_EQ("Starred", starred.suggested_name);

	auto default_case = AssCompat::AnalyzeStyleName("*default");
	EXPECT_TRUE(default_case.has_compatibility_prefix);
	EXPECT_TRUE(default_case.has_default_case_mismatch);
	EXPECT_TRUE(default_case.NeedsExplicitRepair());
	EXPECT_EQ("Default", default_case.suggested_name);

	auto ordinary = AssCompat::AnalyzeStyleName("foo");
	EXPECT_FALSE(ordinary.has_compatibility_prefix);
	EXPECT_FALSE(ordinary.has_default_case_mismatch);
	EXPECT_FALSE(ordinary.NeedsExplicitRepair());
	EXPECT_EQ("foo", ordinary.suggested_name);
}

TEST(ass_compat_format, formats_canonical_ass_values) {
	EXPECT_EQ("-12", AssCompat::FormatInteger(-12));
	EXPECT_EQ("4294967295", AssCompat::FormatUnsignedInteger(4294967295u));
	EXPECT_EQ("1.25", AssCompat::FormatFloat(1.25));
	EXPECT_EQ("2", AssCompat::FormatFloat(2.0));
	EXPECT_EQ("0", AssCompat::FormatFloat(0.0));
	EXPECT_EQ("0", AssCompat::FormatFloat(-0.0));
	EXPECT_EQ("0", AssCompat::FormatFloat(-0.0004));
	EXPECT_EQ("0", AssCompat::FormatFloat(std::numeric_limits<double>::infinity()));
	EXPECT_EQ("0", AssCompat::FormatFloat(std::numeric_limits<double>::quiet_NaN()));
	EXPECT_EQ("0:00:12.34", AssCompat::FormatTime(12345));
	EXPECT_EQ("12:00:00.005", AssCompat::FormatTime(43200005, true));
	EXPECT_EQ("&H04030201", AssCompat::FormatStyleColor(agi::Color(1, 2, 3, 4)));
	EXPECT_EQ("&H030201&", AssCompat::FormatOverrideColor(agi::Color(1, 2, 3, 4)));
	EXPECT_EQ("&HFF&", AssCompat::FormatOverrideAlpha(300));
}

TEST(ass_compat_parse, rejects_non_finite_floats) {
	double value = 1.0;

	EXPECT_FALSE(AssCompat::ParseFloat("nan", value));
	EXPECT_FALSE(AssCompat::ParseFloat("inf", value));
	EXPECT_EQ(1.0, value);
}

TEST(ass_io_core, ass_writer_canonicalizes_script_type) {
	AssFile file;
	file.Info.emplace_back("Title", "ScriptType test");
	file.Info.emplace_back("ScriptType", "generated by tool v4.00+");

	auto path = std::filesystem::temp_directory_path() / "aegisub-script-type-canonical.ass";
	WriteAssFileForCore(&file, path, agi::vfr::Framerate(), "UTF-8", AssWriteOptions{});

	std::string text;
	{
		std::ifstream input(path, std::ios::binary);
		text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	}
	std::filesystem::remove(path);

	EXPECT_NE(std::string::npos, text.find("ScriptType: v4.00+"));
	EXPECT_EQ(std::string::npos, text.find("generated by tool"));
}

TEST(ass_io_core, ass_writer_uses_legacy_rounding_instead_of_frame_safe_projection) {
	AssFile file;
	file.Info.emplace_back("ScriptType", "v4.00+");
	AssStyle style;
	file.Styles.push_back(style);

	AssDialogue line;
	line.Start = 18;
	line.End = 19;
	line.Style = "Default";
	line.Text = "short";
	file.Events.push_back(line);

	auto path = std::filesystem::temp_directory_path() / "aegisub-ass-writer-legacy-time.ass";
	WriteAssFileForCore(&file, path, agi::vfr::Framerate(100.0), "UTF-8", AssWriteOptions{});

	std::string text;
	{
		std::ifstream input(path, std::ios::binary);
		text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	}
	std::filesystem::remove(path);

	EXPECT_NE(std::string::npos, text.find("Dialogue: 0,0:00:00.02,0:00:00.02,Default,,0,0,0,,short"));
	EXPECT_EQ(std::string::npos, text.find("Dialogue: 0,0:00:00.01,0:00:00.02,Default,,0,0,0,,short"));
}

TEST(ass_io_core, ass_writer_roundtrip_preserves_shared_boundary_display) {
	AssFile file;
	file.Info.emplace_back("ScriptType", "v4.00+");
	AssStyle style;
	file.Styles.push_back(style);

	AssDialogue first;
	first.Start = 0;
	first.End = 18;
	first.Style = "Default";
	first.Text = "first";
	file.Events.push_back(first);

	AssDialogue second;
	second.Start = 18;
	second.End = 30;
	second.Style = "Default";
	second.Text = "second";
	file.Events.push_back(second);

	auto path = std::filesystem::temp_directory_path() / "aegisub-ass-writer-shared-boundary.ass";
	WriteAssFileForCore(&file, path, agi::vfr::Framerate(100.0), "UTF-8", AssWriteOptions{});

	std::string text;
	{
		std::ifstream input(path, std::ios::binary);
		text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	}

	AssFile parsed = ReadAssFileForCore(path, "UTF-8");
	std::filesystem::remove(path);

	EXPECT_NE(std::string::npos, text.find("Dialogue: 0,0:00:00.00,0:00:00.02,Default,,0,0,0,,first"));
	EXPECT_NE(std::string::npos, text.find("Dialogue: 0,0:00:00.02,0:00:00.03,Default,,0,0,0,,second"));

	ASSERT_EQ(2, std::distance(parsed.Events.begin(), parsed.Events.end()));
	auto line = parsed.Events.begin();
	auto const& parsed_first = *line++;
	auto const& parsed_second = *line;

	EXPECT_EQ(parsed_first.End.GetMillisecond(), parsed_second.Start.GetMillisecond());
	EXPECT_EQ(20, parsed_first.End.GetMillisecond());
	EXPECT_EQ("0:00:00.02", parsed_first.End.GetAssFormatted());
	EXPECT_EQ(parsed_first.End.GetAssFormatted(), parsed_second.Start.GetAssFormatted());
}
