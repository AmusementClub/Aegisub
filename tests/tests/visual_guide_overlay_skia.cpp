#include <main.h>

#include "../../src/skia/skia_video_overlay_command_buffer.h"
#include "../../src/visual_guide_overlay.h"

namespace {
wxSize MeasureText(std::string const& text, VideoOverlayTextStyle const& style) {
	return wxSize(static_cast<int>(text.size()) * style.size / 2, style.size + 4);
}

VisualGuideViewport MakeViewport() {
	return {
		100.0, 50.0, 800.0, 400.0,
		400.0, 200.0,
	};
}

SkiaVideoOverlayCommandBuffer RecordMeasurement(double second_x) {
	VisualGuide guide;
	guide.id = "guide-1";
	guide.first = { 100.0, 50.0 };
	guide.second = { second_x, 100.0 };

	VisualGuideSnapshot snapshot;
	snapshot.guides.push_back(guide);
	SkiaVideoOverlayRecorder recorder(MeasureText, 1.0f);
	VisualGuideOverlay().Draw(
		recorder,
		MakeViewport(),
		VisualGuideSnapshotView{ snapshot.generation, snapshot.guides, snapshot.selected_id },
		{});
	return recorder.TakeBuffer();
}
}

TEST(visual_guide_overlay_skia, records_normal_bounds_without_invert_backing) {
	auto commands = RecordMeasurement(200.0);
	auto const bounds = commands.NormalBounds();

	EXPECT_FALSE(commands.Empty());
	EXPECT_TRUE(commands.HasNormalContent());
	EXPECT_FALSE(commands.HasInvertContent());
	ASSERT_TRUE(bounds.valid);
	EXPECT_GE(bounds.left, 100.0f);
	EXPECT_GE(bounds.top, 50.0f);
	EXPECT_LE(bounds.right, 900.0f);
	EXPECT_LE(bounds.bottom, 450.0f);
}

TEST(visual_guide_overlay_skia, stable_snapshot_is_equivalent_and_geometry_changes_invalidate_cache) {
	auto first = RecordMeasurement(200.0);
	auto same = RecordMeasurement(200.0);
	auto moved = RecordMeasurement(210.0);

	EXPECT_TRUE(first.EquivalentTo(same));
	EXPECT_FALSE(first.EquivalentTo(moved));
}
