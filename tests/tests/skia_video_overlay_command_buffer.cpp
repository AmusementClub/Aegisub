#include <main.h>

#include "../../src/skia/skia_video_overlay_command_buffer.h"

namespace {
wxSize MeasureTestText(std::string const& text, VideoOverlayTextStyle const& style) {
	return wxSize(static_cast<int>(text.size()) * style.size / 2, style.size + 4);
}

SkiaVideoOverlayCommandBuffer RecordLine(float x) {
	SkiaVideoOverlayRecorder recorder(MeasureTestText, 1.0f);
	recorder.SetLineColour(*wxWHITE, 1.0f, 2);
	recorder.DrawLine(Vector2D(x, 10.0f), Vector2D(x + 20.0f, 30.0f));
	return recorder.TakeBuffer();
}
}

TEST(skia_video_overlay_command_buffer, replay_preserves_owned_commands_exactly) {
	SkiaVideoOverlayRecorder recorder(MeasureTestText, 2.0f);
	recorder.SetLineColour(wxColour(10, 20, 30, 200), 0.75f, 3);
	recorder.SetFillColour(wxColour(40, 50, 60, 220), 0.5f);
	recorder.DrawLine(Vector2D(1.0f, 2.0f), Vector2D(3.0f, 4.0f));
	float lines[] = { 1.0f, 2.0f, 5.0f, 6.0f };
	recorder.DrawLines(2, lines, 2);
	Vector2D strip[] = {
		Vector2D(2.0f, 3.0f),
		Vector2D(4.0f, 5.0f),
		Vector2D(8.0f, 9.0f),
	};
	recorder.DrawLineStrip(strip, 3);
	recorder.DrawRectangle(Vector2D(4.0f, 6.0f), Vector2D(10.0f, 12.0f));
	recorder.DrawPolygon(strip, 3);
	recorder.DrawMultiPolygon(
		{ 2.0f, 2.0f, 8.0f, 2.0f, 8.0f, 8.0f },
		{ 0 },
		{ 3 },
		Vector2D(0.0f, 0.0f),
		Vector2D(100.0f, 80.0f),
		false);
	recorder.DrawCircle(Vector2D(12.0f, 14.0f), 4.0f);
	recorder.DrawTriangle(Vector2D(1.0f, 1.0f), Vector2D(5.0f, 1.0f), Vector2D(3.0f, 4.0f));
	VideoOverlayTextStyle style;
	style.face = "test";
	style.size = 16;
	style.bold = true;
	recorder.DrawText("overlay", 20, 30, style);
	recorder.SetInvert();
	recorder.DrawLine(Vector2D(7.0f, 8.0f), Vector2D(9.0f, 10.0f));
	recorder.ClearInvert();
	auto original = recorder.TakeBuffer();

	lines[0] = 999.0f;
	strip[0] = Vector2D(999.0f, 999.0f);

	SkiaVideoOverlayRecorder replayed_recorder(MeasureTestText, 2.0f);
	original.Replay(replayed_recorder);
	auto replayed = replayed_recorder.TakeBuffer();

	EXPECT_TRUE(original.EquivalentTo(replayed));
	EXPECT_EQ(original.CommandCount(), replayed.CommandCount());
	EXPECT_TRUE(original.HasNormalContent());
	EXPECT_TRUE(original.HasInvertContent());
}

TEST(skia_video_overlay_command_buffer, equivalent_content_reuses_only_exact_commands) {
	auto first = RecordLine(10.0f);
	auto same = RecordLine(10.0f);
	auto moved = RecordLine(11.0f);

	EXPECT_TRUE(first.EquivalentTo(same));
	EXPECT_FALSE(first.EquivalentTo(moved));
}

TEST(skia_video_overlay_command_buffer, invert_and_normal_bounds_are_independent) {
	SkiaVideoOverlayRecorder recorder(MeasureTestText, 1.0f);
	recorder.SetLineColour(*wxWHITE, 1.0f, 2);
	recorder.DrawLine(Vector2D(10.0f, 20.0f), Vector2D(30.0f, 40.0f));
	recorder.SetInvert();
	recorder.DrawLine(Vector2D(100.0f, 120.0f), Vector2D(130.0f, 140.0f));
	auto buffer = recorder.TakeBuffer();

	auto const normal = buffer.NormalBounds();
	auto const invert = buffer.InvertBounds();
	ASSERT_TRUE(normal.valid);
	ASSERT_TRUE(invert.valid);
	EXPECT_LT(normal.right, invert.left);
	EXPECT_TRUE(buffer.CombinedBounds().valid);
}

TEST(skia_video_overlay_command_buffer, inverse_multi_polygon_uses_full_video_bounds) {
	SkiaVideoOverlayRecorder recorder(MeasureTestText, 1.0f);
	recorder.SetFillColour(*wxWHITE, 1.0f);
	recorder.SetLineColour(*wxBLACK, 0.0f, 1);
	recorder.DrawMultiPolygon(
		{ 40.0f, 40.0f, 50.0f, 40.0f, 50.0f, 50.0f },
		{ 0 },
		{ 3 },
		Vector2D(10.0f, 20.0f),
		Vector2D(200.0f, 100.0f),
		true);
	auto buffer = recorder.TakeBuffer();
	auto const bounds = buffer.NormalBounds();

	ASSERT_TRUE(bounds.valid);
	EXPECT_LE(bounds.left, 10.0f);
	EXPECT_LE(bounds.top, 20.0f);
	EXPECT_GE(bounds.right, 210.0f);
	EXPECT_GE(bounds.bottom, 120.0f);
}

TEST(skia_video_overlay_command_buffer, device_bounds_scale_pad_and_clip) {
	SkiaOverlayLogicalBounds logical { true, -2.0f, 10.25f, 40.1f, 80.0f };
	auto const planned = PlanSkiaOverlayDeviceBounds(logical, 2.0f, 100, 120);

	EXPECT_EQ(0, planned.x);
	EXPECT_EQ(19, planned.y);
	EXPECT_EQ(82, planned.width);
	EXPECT_EQ(101, planned.height);
}

TEST(skia_video_overlay_command_buffer, fully_clipped_bounds_produce_empty_target) {
	SkiaOverlayLogicalBounds logical { true, 200.0f, 200.0f, 220.0f, 220.0f };
	auto const planned = PlanSkiaOverlayDeviceBounds(logical, 1.0f, 100, 100);
	EXPECT_TRUE(planned.IsEmpty());
}

TEST(skia_video_overlay_command_buffer, allocation_bounds_align_outward_without_clipping) {
	auto const aligned = AlignSkiaOverlayDeviceBoundsForAllocation(
		{ 37, 41, 20, 18 },
		100,
		100,
		32);

	EXPECT_EQ((SkiaOverlayDeviceBounds { 32, 32, 32, 32 }), aligned);
}

TEST(skia_video_overlay_command_buffer, allocation_alignment_clips_at_canvas_edge) {
	auto const aligned = AlignSkiaOverlayDeviceBoundsForAllocation(
		{ 95, 95, 5, 5 },
		100,
		100,
		32);

	EXPECT_EQ((SkiaOverlayDeviceBounds { 64, 64, 36, 36 }), aligned);
}

TEST(skia_video_overlay_command_buffer, backing_bounds_add_guard_and_reuse_capacity) {
	auto const first = SelectSkiaOverlayBackingBounds(
		{ 100, 100, 20, 20 },
		{},
		640,
		480);
	EXPECT_EQ((SkiaOverlayDeviceBounds { 32, 32, 160, 160 }), first);

	auto const contained = SelectSkiaOverlayBackingBounds(
		{ 110, 110, 20, 20 },
		first,
		640,
		480);
	EXPECT_EQ(first, contained);

	auto const moved = SelectSkiaOverlayBackingBounds(
		{ 195, 110, 20, 20 },
		first,
		640,
		480);
	EXPECT_EQ(first.width, moved.width);
	EXPECT_EQ(first.height, moved.height);
	EXPECT_LE(moved.x, 195);
	EXPECT_GE(moved.x + moved.width, 215);
}

TEST(skia_video_overlay_command_buffer, backing_bounds_clip_to_canvas_edges) {
	auto const left = SelectSkiaOverlayBackingBounds(
		{ 0, 20, 10, 10 },
		{},
		100,
		80);
	EXPECT_EQ((SkiaOverlayDeviceBounds { 0, 0, 96, 80 }), left);

	auto const corner = SelectSkiaOverlayBackingBounds(
		{ 95, 75, 5, 5 },
		{},
		100,
		80);
	EXPECT_EQ((SkiaOverlayDeviceBounds { 0, 0, 100, 80 }), corner);
}

TEST(skia_video_overlay_command_buffer, backing_bounds_grow_only_when_capacity_is_too_small) {
	auto const initial = SelectSkiaOverlayBackingBounds(
		{ 200, 150, 20, 20 },
		{},
		800,
		600);
	auto const grown = SelectSkiaOverlayBackingBounds(
		{ 180, 140, initial.width + 1, initial.height + 1 },
		initial,
		800,
		600);

	EXPECT_GT(grown.width, initial.width);
	EXPECT_GT(grown.height, initial.height);
	auto const smaller = SelectSkiaOverlayBackingBounds(
		{ 300, 200, 10, 10 },
		grown,
		800,
		600);
	EXPECT_EQ(grown.width, smaller.width);
	EXPECT_EQ(grown.height, smaller.height);
}

TEST(skia_video_overlay_command_buffer, backing_capacity_stays_constant_while_content_moves) {
	SkiaOverlayDeviceBounds previous;
	int allocation_count = 0;
	for (int x = 0; x <= 720; ++x) {
		int const y = x * 560 / 720;
		SkiaOverlayDeviceBounds const required { x, y, 80, 40 };
		auto const selected = SelectSkiaOverlayBackingBounds(
			required,
			previous,
			800,
			600);

		ASSERT_FALSE(selected.IsEmpty());
		EXPECT_GE(selected.x, 0);
		EXPECT_GE(selected.y, 0);
		EXPECT_LE(selected.x + selected.width, 800);
		EXPECT_LE(selected.y + selected.height, 600);
		EXPECT_LE(selected.x, required.x);
		EXPECT_LE(selected.y, required.y);
		EXPECT_GE(selected.x + selected.width, required.x + required.width);
		EXPECT_GE(selected.y + selected.height, required.y + required.height);
		if (selected.width != previous.width || selected.height != previous.height)
			++allocation_count;
		previous = selected;
	}

	EXPECT_EQ(1, allocation_count);
}

TEST(skia_video_overlay_command_buffer, backing_bounds_reject_invalid_requests) {
	EXPECT_TRUE(SelectSkiaOverlayBackingBounds({}, {}, 100, 100).IsEmpty());
	EXPECT_TRUE(SelectSkiaOverlayBackingBounds({ 0, 0, 10, 10 }, {}, 0, 100).IsEmpty());
	EXPECT_TRUE(SelectSkiaOverlayBackingBounds({ 200, 200, 10, 10 }, {}, 100, 100).IsEmpty());
	EXPECT_TRUE(SelectSkiaOverlayBackingBounds({ 0, 0, 10, 10 }, {}, 100, 100, 0).IsEmpty());
	EXPECT_TRUE(SelectSkiaOverlayBackingBounds({ 0, 0, 10, 10 }, {}, 100, 100, 32, -1).IsEmpty());
}

TEST(skia_video_overlay_command_buffer, backing_bounds_discard_capacity_outside_resized_canvas) {
	auto const previous = SkiaOverlayDeviceBounds { 0, 0, 640, 480 };
	auto const selected = SelectSkiaOverlayBackingBounds(
		{ 20, 20, 40, 40 },
		previous,
		320,
		240);

	EXPECT_LE(selected.width, 320);
	EXPECT_LE(selected.height, 240);
	EXPECT_GE(selected.x, 0);
	EXPECT_GE(selected.y, 0);
	EXPECT_LE(selected.x + selected.width, 320);
	EXPECT_LE(selected.y + selected.height, 240);
	EXPECT_LE(selected.x, 20);
	EXPECT_GE(selected.x + selected.width, 60);
	EXPECT_LE(selected.y, 20);
	EXPECT_GE(selected.y + selected.height, 60);
}
