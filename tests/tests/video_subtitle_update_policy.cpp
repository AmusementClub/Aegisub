#include <main.h>

#include "../../src/video_subtitle_update_policy.h"

TEST(video_subtitle_update_policy, incremental_rows_are_sorted_unique_and_latest_state_bounded) {
	video_subtitle_update_policy::UpdateCoalescer updates;
	int first[] = { 7, 2, 7 };
	int second[] = { 3, 2 };

	updates.AddIncrementalRows(first);
	updates.AddIncrementalRows(second);
	auto result = updates.Take();

	ASSERT_TRUE(result);
	EXPECT_EQ(video_subtitle_update_policy::UpdateMode::IncrementalLines, result->mode);
	EXPECT_EQ((std::vector<int>{2, 3, 7}), result->rows);
	EXPECT_TRUE(updates.Empty());
}

TEST(video_subtitle_update_policy, full_reload_supersedes_incremental_rows) {
	video_subtitle_update_policy::UpdateCoalescer updates;
	int rows[] = { 1, 4 };

	updates.AddIncrementalRows(rows);
	updates.AddFullReload();
	updates.AddIncrementalRows(rows);
	auto result = updates.Take();

	ASSERT_TRUE(result);
	EXPECT_EQ(video_subtitle_update_policy::UpdateMode::FullReload, result->mode);
	EXPECT_TRUE(result->rows.empty());
}

TEST(video_subtitle_update_policy, invalid_or_empty_incremental_metadata_falls_back_to_full_reload) {
	video_subtitle_update_policy::UpdateCoalescer invalid;
	int rows[] = { 1, -1 };
	invalid.AddIncrementalRows(rows);
	auto invalid_result = invalid.Take();
	ASSERT_TRUE(invalid_result);
	EXPECT_EQ(video_subtitle_update_policy::UpdateMode::FullReload, invalid_result->mode);

	video_subtitle_update_policy::UpdateCoalescer empty;
	empty.AddIncrementalRows({});
	auto empty_result = empty.Take();
	ASSERT_TRUE(empty_result);
	EXPECT_EQ(video_subtitle_update_policy::UpdateMode::FullReload, empty_result->mode);
}
