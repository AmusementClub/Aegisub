#include <main.h>

#include "../../src/visual_frame_visibility_index.h"

#include <initializer_list>
#include <set>
#include <vector>

namespace visibility = aegisub::visual_frame_visibility;

namespace {

std::set<int *> AsSet(std::initializer_list<int *> lines) {
	return std::set<int *>(lines);
}

}

TEST(visual_frame_visibility_index, rebuild_tracks_inclusive_frame_intervals) {
	int first = 1;
	int second = 2;
	int comment = 3;
	visibility::Index<int> index;
	index.Rebuild({
		{2, 4, &first},
		{4, 6, &second},
		{5, 4, &comment},
	}, 4);

	EXPECT_EQ(AsSet({&first, &second}), index.Visible());
}

TEST(visual_frame_visibility_index, forward_seek_reports_crossed_boundaries) {
	int first = 1;
	int second = 2;
	visibility::Index<int> index;
	index.Rebuild({{2, 4, &first}, {4, 6, &second}}, 3);
	std::vector<int *> entered;
	std::vector<int *> exited;

	index.Advance(3, 5, entered, exited);

	EXPECT_EQ(AsSet({&second}), std::set<int *>(entered.begin(), entered.end()));
	EXPECT_EQ(AsSet({&first}), std::set<int *>(exited.begin(), exited.end()));
	EXPECT_EQ(AsSet({&second}), index.Visible());
}

TEST(visual_frame_visibility_index, backward_seek_reports_crossed_boundaries) {
	int first = 1;
	int second = 2;
	visibility::Index<int> index;
	index.Rebuild({{2, 4, &first}, {4, 6, &second}}, 5);
	std::vector<int *> entered;
	std::vector<int *> exited;

	index.Advance(5, 3, entered, exited);

	EXPECT_EQ(AsSet({&first}), std::set<int *>(entered.begin(), entered.end()));
	EXPECT_EQ(AsSet({&second}), std::set<int *>(exited.begin(), exited.end()));
	EXPECT_EQ(AsSet({&first}), index.Visible());
}

TEST(visual_frame_visibility_index, single_frame_steps_keep_last_frame_visible) {
	int line = 1;
	visibility::Index<int> index;
	index.Rebuild({{2, 4, &line}}, 3);
	std::vector<int *> entered;
	std::vector<int *> exited;

	index.Advance(3, 4, entered, exited);
	EXPECT_TRUE(exited.empty());
	EXPECT_EQ(AsSet({&line}), index.Visible());

	index.Advance(4, 5, entered, exited);
	EXPECT_EQ(AsSet({&line}), std::set<int *>(exited.begin(), exited.end()));
	EXPECT_TRUE(index.Visible().empty());
}

TEST(visual_frame_visibility_index, long_seek_omits_lines_invisible_at_both_ends) {
	int crossed = 1;
	int destination = 2;
	visibility::Index<int> index;
	index.Rebuild({{2, 4, &crossed}, {8, 12, &destination}}, 0);
	std::vector<int *> entered;
	std::vector<int *> exited;

	index.Advance(0, 10, entered, exited);

	EXPECT_EQ(AsSet({&destination}), std::set<int *>(entered.begin(), entered.end()));
	EXPECT_TRUE(exited.empty());
	EXPECT_EQ(AsSet({&destination}), index.Visible());
}

TEST(visual_frame_visibility_index, backward_seek_handles_single_frame_interval) {
	int line = 1;
	visibility::Index<int> index;
	index.Rebuild({{3, 3, &line}}, 5);
	std::vector<int *> entered;
	std::vector<int *> exited;

	index.Advance(5, 3, entered, exited);

	EXPECT_EQ(AsSet({&line}), std::set<int *>(entered.begin(), entered.end()));
	EXPECT_TRUE(exited.empty());
	EXPECT_EQ(AsSet({&line}), index.Visible());
}

TEST(visual_frame_visibility_index, dense_boundary_reports_every_line_once) {
	std::vector<int> lines(2048);
	std::vector<visibility::Interval<int>> intervals;
	intervals.reserve(lines.size());
	for (auto& line : lines)
		intervals.push_back({3, 3, &line});

	visibility::Index<int> index;
	index.Rebuild(intervals, 2);
	std::vector<int *> entered;
	std::vector<int *> exited;

	index.Advance(2, 3, entered, exited);
	EXPECT_EQ(lines.size(), entered.size());
	EXPECT_TRUE(exited.empty());
	EXPECT_EQ(lines.size(), index.Visible().size());
	EXPECT_EQ(
		std::set<int *>(entered.begin(), entered.end()).size(),
		entered.size());

	index.Advance(3, 4, entered, exited);
	EXPECT_TRUE(entered.empty());
	EXPECT_EQ(lines.size(), exited.size());
	EXPECT_TRUE(index.Visible().empty());
	EXPECT_EQ(
		std::set<int *>(exited.begin(), exited.end()).size(),
		exited.size());
}
