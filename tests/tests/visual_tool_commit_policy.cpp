#include <main.h>

#include "../../src/visual_tool_commit_policy.h"

namespace {
using visual_tool_commit_policy::RefreshInput;
using visual_tool_commit_policy::ShouldRefreshFile;
using visual_tool_commit_policy::UsesChangedLineFilter;
}

TEST(visual_tool_commit_policy, forced_external_refresh_survives_unrelated_line_filter) {
	RefreshInput input;
	input.commit_type = AssFile::COMMIT_DIAG_TEXT;
	input.refresh_any_external_commit = true;
	input.has_changed_line = true;
	input.changed_line_relevant = false;
	EXPECT_TRUE(ShouldRefreshFile(input));

	input.commit_type = AssFile::COMMIT_EXTRADATA;
	EXPECT_TRUE(ShouldRefreshFile(input));

	input.commit_type = AssFile::COMMIT_ATTACHMENT;
	EXPECT_TRUE(ShouldRefreshFile(input));
}

TEST(visual_tool_commit_policy, ordinary_tools_keep_active_and_displayed_line_filter) {
	RefreshInput input;
	input.commit_type = AssFile::COMMIT_DIAG_TEXT;
	input.has_changed_line = true;
	EXPECT_FALSE(ShouldRefreshFile(input));

	input.changed_line_relevant = true;
	EXPECT_TRUE(ShouldRefreshFile(input));

	input.changed_line_relevant = false;
	input.has_changed_line = false;
	EXPECT_TRUE(ShouldRefreshFile(input));

	input.commit_type = AssFile::COMMIT_EXTRADATA;
	input.has_changed_line = true;
	EXPECT_FALSE(ShouldRefreshFile(input));
	input.changed_line_relevant = true;
	EXPECT_TRUE(ShouldRefreshFile(input));
	input.has_changed_line = false;
	input.changed_line_relevant = false;
	EXPECT_TRUE(ShouldRefreshFile(input));
}

TEST(visual_tool_commit_policy, forced_refresh_applies_only_to_external_non_coordinate_commits) {
	RefreshInput input;
	input.commit_type = AssFile::COMMIT_DIAG_TEXT;
	input.refresh_any_external_commit = true;
	input.has_changed_line = true;
	input.local_commit = true;
	EXPECT_FALSE(ShouldRefreshFile(input));
	input.changed_line_relevant = true;
	EXPECT_TRUE(ShouldRefreshFile(input));

	input.local_commit = false;
	input.changed_line_relevant = false;
	input.commit_type = AssFile::COMMIT_NEW;
	EXPECT_FALSE(ShouldRefreshFile(input));

	input.commit_type = AssFile::COMMIT_SCRIPTINFO;
	EXPECT_FALSE(ShouldRefreshFile(input));
	input.commit_type = AssFile::COMMIT_SCRIPTINFO | AssFile::COMMIT_DIAG_TEXT;
	EXPECT_FALSE(ShouldRefreshFile(input));
	input.changed_line_relevant = true;
	EXPECT_TRUE(ShouldRefreshFile(input));
	input.commit_type = AssFile::COMMIT_SCRIPTINFO | AssFile::COMMIT_STYLES;
	input.changed_line_relevant = false;
	EXPECT_TRUE(ShouldRefreshFile(input));
}

TEST(visual_tool_commit_policy, broad_file_changes_always_refresh) {
	for (int commit_type : {
		AssFile::COMMIT_STYLES,
		AssFile::COMMIT_ORDER,
		AssFile::COMMIT_DIAG_ADDREM,
		AssFile::COMMIT_DIAG_META,
		AssFile::COMMIT_DIAG_TIME,
	}) {
		RefreshInput input;
		input.commit_type = commit_type;
		input.local_commit = true;
		EXPECT_TRUE(ShouldRefreshFile(input)) << commit_type;
	}
}

TEST(visual_tool_commit_policy, changed_line_filter_is_not_used_for_broad_commits) {
	EXPECT_TRUE(UsesChangedLineFilter(AssFile::COMMIT_DIAG_TEXT));
	EXPECT_TRUE(UsesChangedLineFilter(AssFile::COMMIT_EXTRADATA));
	EXPECT_FALSE(UsesChangedLineFilter(AssFile::COMMIT_ATTACHMENT));
	EXPECT_FALSE(UsesChangedLineFilter(AssFile::COMMIT_DIAG_ADDREM));
	EXPECT_FALSE(UsesChangedLineFilter(
		AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_TEXT));
}
