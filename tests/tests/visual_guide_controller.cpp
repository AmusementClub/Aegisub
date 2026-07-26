#include <main.h>

#include "../../src/visual_guide_controller.h"

#include <limits>
#include <type_traits>
#include <utility>

namespace {
VisualGuide MakeGuide() {
	VisualGuide guide;
	guide.first = { 10.0, 20.0 };
	guide.second = { 30.0, 40.0 };
	return guide;
}
}

TEST(visual_guide_controller, add_assigns_monotonic_non_reused_ids) {
	VisualGuideController controller;

	auto const first = controller.Add(MakeGuide());
	ASSERT_TRUE(first);
	EXPECT_EQ("guide-1", *first);
	EXPECT_TRUE(controller.Remove(*first));

	auto const second = controller.Add(MakeGuide());
	ASSERT_TRUE(second);
	EXPECT_EQ("guide-2", *second);

	auto const snapshot = controller.CaptureSnapshot();
	EXPECT_EQ(4u, snapshot.generation);
	ASSERT_EQ(1u, snapshot.guides.size());
	EXPECT_EQ("guide-2", snapshot.guides.front().id);
}

TEST(visual_guide_controller, mutations_signal_and_no_op_operations_do_not_advance_generation) {
	VisualGuideController controller;
	int change_count = 0;
	auto connection = controller.AddChangedListener([&] { ++change_count; });

	auto const id = controller.Add(MakeGuide());
	ASSERT_TRUE(id);
	EXPECT_EQ(2u, controller.CaptureSnapshot().generation);
	EXPECT_EQ(1, change_count);

	EXPECT_TRUE(controller.Select(*id));
	EXPECT_EQ(3u, controller.CaptureSnapshot().generation);
	EXPECT_EQ(2, change_count);
	EXPECT_FALSE(controller.Select(*id));
	EXPECT_FALSE(controller.Update(*id, MakeGuide()));
	EXPECT_FALSE(controller.Select(std::string("missing")));
	EXPECT_EQ(3u, controller.CaptureSnapshot().generation);
	EXPECT_EQ(2, change_count);

	VisualGuide changed = MakeGuide();
	changed.second.x = 31.0;
	EXPECT_TRUE(controller.Update(*id, changed));
	EXPECT_EQ(4u, controller.CaptureSnapshot().generation);
	EXPECT_EQ(3, change_count);
	EXPECT_TRUE(controller.Remove(*id));
	EXPECT_EQ(5u, controller.CaptureSnapshot().generation);
	EXPECT_EQ(4, change_count);
	controller.Clear();
	EXPECT_EQ(5u, controller.CaptureSnapshot().generation);
	EXPECT_EQ(4, change_count);

	ASSERT_TRUE(controller.Add(MakeGuide()));
	EXPECT_EQ(6u, controller.CaptureSnapshot().generation);
	EXPECT_EQ(5, change_count);
	controller.Clear();
	EXPECT_EQ(7u, controller.CaptureSnapshot().generation);
	EXPECT_EQ(6, change_count);
	controller.Clear();
	EXPECT_EQ(7u, controller.CaptureSnapshot().generation);
	EXPECT_EQ(6, change_count);
}

TEST(visual_guide_controller, selection_and_last_measurement_track_current_guides) {
	VisualGuideController controller;
	auto const first = controller.Add(MakeGuide());
	ASSERT_TRUE(first);
	auto const second = controller.Add(MakeGuide());
	ASSERT_TRUE(second);

	EXPECT_TRUE(controller.Select(*second));
	auto snapshot = controller.CaptureSnapshot();
	EXPECT_EQ(*second, *snapshot.selected_id);
	EXPECT_EQ(*second, *snapshot.last_measurement_id);

	EXPECT_TRUE(controller.Remove(*second));
	snapshot = controller.CaptureSnapshot();
	EXPECT_FALSE(snapshot.selected_id);
	EXPECT_EQ(*first, *snapshot.last_measurement_id);

	EXPECT_TRUE(controller.Remove(*first));
	snapshot = controller.CaptureSnapshot();
	EXPECT_FALSE(snapshot.last_measurement_id);
}

TEST(visual_guide_controller, invalid_updates_leave_existing_state_unchanged) {
	VisualGuideController controller;
	auto const id = controller.Add(MakeGuide());
	ASSERT_TRUE(id);
	auto const before = controller.CaptureSnapshot();

	VisualGuide invalid = MakeGuide();
	invalid.second.y = std::numeric_limits<double>::infinity();
	EXPECT_FALSE(controller.Update(*id, invalid));
	EXPECT_FALSE(controller.Update("", MakeGuide()));
	EXPECT_FALSE(controller.Update("missing", MakeGuide()));

	auto const after = controller.CaptureSnapshot();
	EXPECT_EQ(before.generation, after.generation);
	EXPECT_EQ(before.guides, after.guides);
}

TEST(visual_guide_controller, maximum_count_is_enforced_without_discarding_existing_guides) {
	VisualGuideController controller;
	for (size_t index = 0; index < VisualGuideController::MaximumGuideCount; ++index)
		ASSERT_TRUE(controller.Add(MakeGuide()));

	EXPECT_FALSE(controller.Add(MakeGuide()));
	auto const snapshot = controller.CaptureSnapshot();
	EXPECT_EQ(VisualGuideController::MaximumGuideCount, snapshot.guides.size());
	EXPECT_EQ("guide-256", snapshot.guides.back().id);
	EXPECT_EQ(257u, snapshot.generation);
}

TEST(visual_guide_controller, snapshot_is_a_value_copy_and_reset_clears_current_video_guides) {
	VisualGuideController controller;
	auto const id = controller.Add(MakeGuide());
	ASSERT_TRUE(id);
	auto snapshot = controller.CaptureSnapshot();
	ASSERT_EQ(1u, snapshot.guides.size());
	snapshot.guides.front().first.x = 999.0;
	snapshot.selected_id = "different";

	auto const unchanged = controller.CaptureSnapshot();
	EXPECT_DOUBLE_EQ(10.0, unchanged.guides.front().first.x);
	EXPECT_FALSE(unchanged.selected_id);

	controller.ResetForVideoChange();
	auto const reset = controller.CaptureSnapshot();
	EXPECT_TRUE(reset.guides.empty());
	EXPECT_FALSE(reset.selected_id);
	EXPECT_FALSE(reset.last_measurement_id);
}

TEST(visual_guide_controller, view_observes_current_state_without_copying_it) {
	static_assert(noexcept(std::declval<VisualGuideController const&>().CaptureView()));
	static_assert(std::is_reference_v<decltype(VisualGuideSnapshotView::generation)>);
	static_assert(std::is_reference_v<decltype(VisualGuideSnapshotView::guides)>);
	static_assert(std::is_reference_v<decltype(VisualGuideSnapshotView::selected_id)>);

	VisualGuideController controller;
	auto const id = controller.Add(MakeGuide());
	ASSERT_TRUE(id);
	auto const view = controller.CaptureView();

	EXPECT_EQ(2u, view.generation);
	ASSERT_EQ(1u, view.guides.size());
	EXPECT_FALSE(view.selected_id);
	EXPECT_TRUE(controller.Select(*id));
	EXPECT_EQ(*id, *controller.CaptureView().selected_id);
}
