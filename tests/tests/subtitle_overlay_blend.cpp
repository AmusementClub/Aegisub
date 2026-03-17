#include <main.h>

#include "../../src/subtitle_overlay_blend.h"

#include <cstring>

namespace {
void apply_dirty_rects(SubtitleOverlayStorage const& src, SubtitleOverlayStorage& dst) {
	for (auto const& rect : src.dirty_rects) {
		for (int y = 0; y < rect.height; ++y) {
			auto const* src_row = src.pixels.data() + static_cast<std::ptrdiff_t>(rect.y + y) * src.pitch + static_cast<std::ptrdiff_t>(rect.x) * 4;
			auto* dst_row = dst.pixels.data() + static_cast<std::ptrdiff_t>(rect.y + y) * dst.pitch + static_cast<std::ptrdiff_t>(rect.x) * 4;
			std::memcpy(dst_row, src_row, static_cast<size_t>(rect.width) * 4);
		}
	}
	dst.has_visible_content = src.has_visible_content;
}

VideoFrame composite_overlay(VideoFrame const& source, SubtitleOverlayStorage const& storage) {
	VideoFrame result = source;
	auto overlay = const_cast<SubtitleOverlayStorage&>(storage).MakeView(true);
	overlay.color_role = SubtitleOverlayColorRole::SubtitleVideoCompatibility;
	CompositePremultipliedBgraOverlayOntoVideoFrame(result, overlay);
	return result;
}

VideoFrame make_frame(int width, int height) {
	VideoFrame frame;
	frame.width = static_cast<size_t>(width);
	frame.height = static_cast<size_t>(height);
	frame.pitch = static_cast<size_t>(width) * 4;
	frame.flipped = false;
	frame.data.assign(frame.pitch * frame.height, 0);
	return frame;
}

void fill_box(VideoFrame& frame, int x0, int y0, int width, int height, unsigned char b, unsigned char g, unsigned char r) {
	for (int y = y0; y < y0 + height; ++y) {
		auto* row = frame.data.data() + static_cast<std::ptrdiff_t>(y) * frame.pitch;
		for (int x = x0; x < x0 + width; ++x) {
			auto* pixel = row + static_cast<std::ptrdiff_t>(x) * 4;
			pixel[0] = b;
			pixel[1] = g;
			pixel[2] = r;
			pixel[3] = 0;
		}
	}
}

void expect_rects_equal(
	std::vector<SubtitleOverlayDirtyRect> const& expected,
	std::vector<SubtitleOverlayDirtyRect> const& actual) {
	ASSERT_EQ(expected.size(), actual.size());
	for (size_t i = 0; i < expected.size(); ++i) {
		EXPECT_EQ(expected[i].x, actual[i].x);
		EXPECT_EQ(expected[i].y, actual[i].y);
		EXPECT_EQ(expected[i].width, actual[i].width);
		EXPECT_EQ(expected[i].height, actual[i].height);
	}
}

void expect_fused_matches_two_pass(
	VideoFrame const& previous_source,
	VideoFrame const& previous_composited,
	VideoFrame const& current_source,
	VideoFrame const& current_composited,
	int tile_width,
	int tile_height) {
	SubtitleOverlayStorage previous_storage;
	SubtitleOverlay previous_overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(
		previous_source,
		previous_composited,
		previous_storage,
		previous_overlay));
	ASSERT_TRUE(BuildDirtyTileRectsForOverlay(nullptr, previous_storage, tile_width, tile_height));

	SubtitleOverlayStorage two_pass_storage;
	SubtitleOverlay two_pass_overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(
		current_source,
		current_composited,
		two_pass_storage,
		two_pass_overlay));
	ASSERT_TRUE(BuildDirtyTileRectsForOverlay(&previous_storage, two_pass_storage, tile_width, tile_height));

	SubtitleOverlayStorage fused_storage;
	SubtitleOverlay fused_overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlayWithDirtyTiles(
		current_source,
		current_composited,
		&previous_storage,
		fused_storage,
		fused_overlay,
		tile_width,
		tile_height));

	EXPECT_EQ(two_pass_storage.pixels, fused_storage.pixels);
	expect_rects_equal(two_pass_storage.dirty_rects, fused_storage.dirty_rects);

	apply_dirty_rects(fused_storage, previous_storage);
	auto reconstructed = composite_overlay(current_source, previous_storage);
	EXPECT_EQ(current_composited.data, reconstructed.data);
}
}

TEST(subtitle_overlay_blend, legacy_bake_in_blends_against_existing_background) {
	unsigned char pixels[] = {
		10, 20, 30, 0
	};
	unsigned char mask[] = { 255 };

	BlendLibassMaskIntoBgraTarget(
		{ pixels, 4, 1, 1, false },
		SubtitleOverlayBlendMode::LegacyBakeIn,
		0,
		0,
		1,
		1,
		mask,
		1,
		0x11223300u);

	EXPECT_EQ(0x33, pixels[0]);
	EXPECT_EQ(0x22, pixels[1]);
	EXPECT_EQ(0x11, pixels[2]);
	EXPECT_EQ(0, pixels[3]);
}

TEST(subtitle_overlay_blend, premultiplied_overlay_writes_alpha_and_color) {
	unsigned char pixels[] = {
		0, 0, 0, 0
	};
	unsigned char mask[] = { 255 };

	BlendLibassMaskIntoBgraTarget(
		{ pixels, 4, 1, 1, false },
		SubtitleOverlayBlendMode::PremultipliedOverlay,
		0,
		0,
		1,
		1,
		mask,
		1,
		0x11223300u);

	EXPECT_EQ(0x33, pixels[0]);
	EXPECT_EQ(0x22, pixels[1]);
	EXPECT_EQ(0x11, pixels[2]);
	EXPECT_EQ(255, pixels[3]);
}

TEST(subtitle_overlay_blend, premultiplied_overlay_accumulates_over_existing_pixel) {
	unsigned char pixels[] = {
		20, 40, 60, 128
	};
	unsigned char mask[] = { 128 };

	BlendLibassMaskIntoBgraTarget(
		{ pixels, 4, 1, 1, false },
		SubtitleOverlayBlendMode::PremultipliedOverlay,
		0,
		0,
		1,
		1,
		mask,
		1,
		0x20406000u);

	EXPECT_EQ(57, pixels[0]);
	EXPECT_EQ(51, pixels[1]);
	EXPECT_EQ(45, pixels[2]);
	EXPECT_EQ(191, pixels[3]);
}

TEST(subtitle_overlay_blend, clear_target_zeros_all_rows_even_when_flipped) {
	unsigned char pixels[] = {
		1, 2, 3, 4,
		5, 6, 7, 8
	};

	ClearBgraSubtitleTarget({ pixels, 4, 1, 2, true });

	for (auto value : pixels)
		EXPECT_EQ(0, value);
}

TEST(subtitle_overlay_blend, extract_difference_overlay_returns_minimal_patch) {
	VideoFrame source;
	source.width = 3;
	source.height = 2;
	source.pitch = 12;
	source.flipped = false;
	source.data.assign(24, 0);

	VideoFrame composited = source;
	composited.data[16] = 9;
	composited.data[17] = 8;
	composited.data[18] = 7;

	SubtitleOverlayStorage storage;
	SubtitleOverlay overlay;
	ASSERT_TRUE(ExtractOpaqueBgraDifferenceOverlay(source, composited, storage, overlay));
	EXPECT_EQ(1, overlay.width);
	EXPECT_EQ(1, overlay.height);
	EXPECT_EQ(1, overlay.target_x);
	EXPECT_EQ(1, overlay.target_y);
	EXPECT_EQ(3, overlay.canvas_width);
	EXPECT_EQ(2, overlay.canvas_height);
	EXPECT_EQ(SubtitleOverlayCompositionMode::OpaqueReplace, overlay.composition_mode);
	EXPECT_EQ(9, overlay.planes[0].data[0]);
	EXPECT_EQ(8, overlay.planes[0].data[1]);
	EXPECT_EQ(7, overlay.planes[0].data[2]);
}

TEST(subtitle_overlay_blend, opaque_replace_overlay_copies_patch_to_target_rect) {
	VideoFrame frame;
	frame.width = 3;
	frame.height = 2;
	frame.pitch = 12;
	frame.flipped = false;
	frame.data.assign(24, 0);

	SubtitleOverlayStorage storage;
	storage.Reset(1, 1, false);
	storage.pixels[0] = 1;
	storage.pixels[1] = 2;
	storage.pixels[2] = 3;
	storage.pixels[3] = 4;

	auto overlay = storage.MakeView(false);
	overlay.canvas_width = 3;
	overlay.canvas_height = 2;
	overlay.target_x = 2;
	overlay.target_y = 1;
	overlay.composition_mode = SubtitleOverlayCompositionMode::OpaqueReplace;

	CompositeOpaqueBgraOverlayOntoVideoFrame(frame, overlay);

	auto offset = static_cast<size_t>(1 * frame.pitch + 2 * 4);
	EXPECT_EQ(1, frame.data[offset + 0]);
	EXPECT_EQ(2, frame.data[offset + 1]);
	EXPECT_EQ(3, frame.data[offset + 2]);
	EXPECT_EQ(4, frame.data[offset + 3]);
}

TEST(subtitle_overlay_blend, sparse_compatibility_overlay_marks_only_changed_pixels) {
	VideoFrame source;
	source.width = 3;
	source.height = 2;
	source.pitch = 12;
	source.flipped = false;
	source.data.assign(24, 0);

	VideoFrame composited = source;
	composited.data[4] = 9;
	composited.data[5] = 8;
	composited.data[6] = 7;

	SubtitleOverlayStorage storage;
	SubtitleOverlay overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(source, composited, storage, overlay));
	EXPECT_TRUE(storage.has_visible_content);
	EXPECT_EQ(3, overlay.width);
	EXPECT_EQ(2, overlay.height);
	EXPECT_EQ(SubtitleOverlayCompositionMode::PremultipliedAlpha, overlay.composition_mode);
	EXPECT_EQ(0, overlay.planes[0].data[0]);
	EXPECT_EQ(9, overlay.planes[0].data[4]);
	EXPECT_EQ(8, overlay.planes[0].data[5]);
	EXPECT_EQ(7, overlay.planes[0].data[6]);
	EXPECT_EQ(255, overlay.planes[0].data[7]);
}

TEST(subtitle_overlay_blend, dirty_tile_rects_capture_changed_tiles_between_surfaces) {
	SubtitleOverlayStorage previous;
	previous.Reset(8, 4, false);
	previous.has_visible_content = true;
	previous.pixels[0] = 1;
	previous.pixels[3] = 255;
	previous.row_ranges[0] = { 0, 1 };
	previous.active_row_begin = 0;
	previous.active_row_end = 1;

	SubtitleOverlayStorage current;
	current.Reset(8, 4, false);
	current.has_visible_content = true;
	current.pixels[64] = 2;
	current.pixels[67] = 255;
	current.row_ranges[2] = { 0, 1 };
	current.active_row_begin = 2;
	current.active_row_end = 3;

	ASSERT_TRUE(BuildDirtyTileRectsForOverlay(&previous, current, 4, 2));
	ASSERT_FALSE(current.dirty_rects.empty());
	for (auto const& rect : current.dirty_rects) {
		EXPECT_EQ(0, rect.x);
		EXPECT_EQ(4, rect.width);
		EXPECT_GE(rect.y, 0);
		EXPECT_LE(rect.y + rect.height, 4);
	}
}

TEST(subtitle_overlay_blend, dirty_tile_updates_roundtrip_to_current_composited_frame) {
	VideoFrame source;
	source.width = 8;
	source.height = 4;
	source.pitch = 32;
	source.flipped = false;
	source.data.assign(128, 0);

	VideoFrame previous_composited = source;
	previous_composited.data[0] = 1;
	previous_composited.data[3] = 0;
	previous_composited.data[76] = 5;
	previous_composited.data[79] = 0;

	VideoFrame current_composited = source;
	current_composited.data[16] = 2;
	current_composited.data[19] = 0;
	current_composited.data[108] = 9;
	current_composited.data[111] = 0;

	SubtitleOverlayStorage previous_storage;
	SubtitleOverlay previous_overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(source, previous_composited, previous_storage, previous_overlay));
	ASSERT_TRUE(BuildDirtyTileRectsForOverlay(nullptr, previous_storage, 4, 2));

	SubtitleOverlayStorage current_storage;
	SubtitleOverlay current_overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(source, current_composited, current_storage, current_overlay));
	ASSERT_TRUE(BuildDirtyTileRectsForOverlay(&previous_storage, current_storage, 4, 2));

	apply_dirty_rects(current_storage, previous_storage);
	auto reconstructed = composite_overlay(source, previous_storage);
	EXPECT_EQ(current_composited.data, reconstructed.data);
}

TEST(subtitle_overlay_blend, dirty_tile_updates_can_clear_previous_subtitle_pixels) {
	VideoFrame source;
	source.width = 8;
	source.height = 4;
	source.pitch = 32;
	source.flipped = false;
	source.data.assign(128, 0);

	VideoFrame previous_composited = source;
	previous_composited.data[32] = 4;
	previous_composited.data[35] = 0;

	VideoFrame current_composited = source;

	SubtitleOverlayStorage previous_storage;
	SubtitleOverlay previous_overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(source, previous_composited, previous_storage, previous_overlay));
	ASSERT_TRUE(BuildDirtyTileRectsForOverlay(nullptr, previous_storage, 4, 2));

	SubtitleOverlayStorage current_storage;
	SubtitleOverlay current_overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(source, current_composited, current_storage, current_overlay));
	ASSERT_TRUE(BuildDirtyTileRectsForOverlay(&previous_storage, current_storage, 4, 2));

	apply_dirty_rects(current_storage, previous_storage);
	auto reconstructed = composite_overlay(source, previous_storage);
	EXPECT_EQ(current_composited.data, reconstructed.data);
}

TEST(subtitle_overlay_blend, dirty_tile_updates_track_subtitle_motion_across_separate_tiles) {
	auto source = make_frame(12, 4);

	auto previous_composited = source;
	fill_box(previous_composited, 1, 0, 2, 1, 32, 64, 96);

	auto current_composited = source;
	fill_box(current_composited, 9, 0, 2, 1, 160, 192, 224);

	SubtitleOverlayStorage previous_storage;
	SubtitleOverlay previous_overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(source, previous_composited, previous_storage, previous_overlay));
	ASSERT_TRUE(BuildDirtyTileRectsForOverlay(nullptr, previous_storage, 4, 2));

	SubtitleOverlayStorage current_storage;
	SubtitleOverlay current_overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(source, current_composited, current_storage, current_overlay));
	ASSERT_TRUE(BuildDirtyTileRectsForOverlay(&previous_storage, current_storage, 4, 2));
	ASSERT_EQ(2u, current_storage.dirty_rects.size());

	apply_dirty_rects(current_storage, previous_storage);
	auto reconstructed = composite_overlay(source, previous_storage);
	EXPECT_EQ(current_composited.data, reconstructed.data);
}

TEST(subtitle_overlay_blend, dirty_tile_updates_track_background_changes_under_stable_subtitle_geometry) {
	auto previous_source = make_frame(8, 4);
	auto previous_composited = previous_source;
	fill_box(previous_composited, 2, 1, 3, 1, 120, 140, 180);

	auto current_source = make_frame(8, 4);
	fill_box(current_source, 2, 1, 3, 1, 8, 16, 24);
	auto current_composited = current_source;
	fill_box(current_composited, 2, 1, 3, 1, 150, 170, 210);

	SubtitleOverlayStorage previous_storage;
	SubtitleOverlay previous_overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(previous_source, previous_composited, previous_storage, previous_overlay));
	ASSERT_TRUE(BuildDirtyTileRectsForOverlay(nullptr, previous_storage, 4, 2));

	SubtitleOverlayStorage current_storage;
	SubtitleOverlay current_overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(current_source, current_composited, current_storage, current_overlay));
	ASSERT_TRUE(BuildDirtyTileRectsForOverlay(&previous_storage, current_storage, 4, 2));
	EXPECT_FALSE(current_storage.dirty_rects.empty());

	apply_dirty_rects(current_storage, previous_storage);
	auto reconstructed = composite_overlay(current_source, previous_storage);
	EXPECT_EQ(current_composited.data, reconstructed.data);
}

TEST(subtitle_overlay_blend, sparse_compatibility_overlay_reuses_surface_and_clears_retired_rows) {
	auto source = make_frame(8, 4);

	auto previous_composited = source;
	fill_box(previous_composited, 1, 0, 2, 1, 50, 80, 110);

	auto current_composited = source;
	fill_box(current_composited, 4, 2, 2, 1, 140, 170, 200);

	SubtitleOverlayStorage storage;
	SubtitleOverlay overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(source, previous_composited, storage, overlay));
	ASSERT_EQ(0, storage.active_row_begin);
	ASSERT_EQ(1, storage.active_row_end);
	ASSERT_FALSE(storage.row_ranges[0].IsEmpty());

	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(source, current_composited, storage, overlay));
	EXPECT_TRUE(storage.row_ranges[0].IsEmpty());
	EXPECT_EQ(2, storage.active_row_begin);
	EXPECT_EQ(3, storage.active_row_end);

	auto old_pixel = static_cast<size_t>(0 * storage.pitch + 1 * 4);
	EXPECT_EQ(0, storage.pixels[old_pixel + 0]);
	EXPECT_EQ(0, storage.pixels[old_pixel + 1]);
	EXPECT_EQ(0, storage.pixels[old_pixel + 2]);
	EXPECT_EQ(0, storage.pixels[old_pixel + 3]);

	auto reconstructed = composite_overlay(source, storage);
	EXPECT_EQ(current_composited.data, reconstructed.data);
}

TEST(subtitle_overlay_blend, fused_sparse_overlay_matches_two_pass_dirty_diff) {
	auto previous_source = make_frame(12, 6);
	auto previous_composited = previous_source;
	fill_box(previous_composited, 1, 1, 3, 1, 80, 90, 100);
	fill_box(previous_composited, 8, 4, 2, 1, 30, 40, 50);

	auto current_source = make_frame(12, 6);
	fill_box(current_source, 1, 1, 3, 1, 5, 6, 7);
	auto current_composited = current_source;
	fill_box(current_composited, 2, 1, 3, 1, 100, 120, 140);
	fill_box(current_composited, 6, 4, 3, 1, 60, 70, 90);

	SubtitleOverlayStorage previous_storage;
	SubtitleOverlay previous_overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(previous_source, previous_composited, previous_storage, previous_overlay));
	ASSERT_TRUE(BuildDirtyTileRectsForOverlay(nullptr, previous_storage, 4, 2));

	SubtitleOverlayStorage two_pass_storage;
	SubtitleOverlay two_pass_overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlay(current_source, current_composited, two_pass_storage, two_pass_overlay));
	ASSERT_TRUE(BuildDirtyTileRectsForOverlay(&previous_storage, two_pass_storage, 4, 2));

	SubtitleOverlayStorage fused_storage;
	SubtitleOverlay fused_overlay;
	ASSERT_TRUE(BuildSparsePremultipliedCompatibilityOverlayWithDirtyTiles(
		current_source,
		current_composited,
		&previous_storage,
		fused_storage,
		fused_overlay,
		4,
		2));

	EXPECT_EQ(two_pass_storage.pixels, fused_storage.pixels);
	expect_rects_equal(two_pass_storage.dirty_rects, fused_storage.dirty_rects);

	apply_dirty_rects(fused_storage, previous_storage);
	auto reconstructed = composite_overlay(current_source, previous_storage);
	EXPECT_EQ(current_composited.data, reconstructed.data);
}

TEST(subtitle_overlay_blend, fused_sparse_overlay_matches_two_pass_when_subtitle_disappears) {
	auto previous_source = make_frame(16, 6);
	auto previous_composited = previous_source;
	fill_box(previous_composited, 2, 1, 5, 1, 220, 220, 220);
	fill_box(previous_composited, 9, 3, 4, 1, 180, 210, 255);

	auto current_source = make_frame(16, 6);
	auto current_composited = current_source;

	expect_fused_matches_two_pass(
		previous_source,
		previous_composited,
		current_source,
		current_composited,
		4,
		2);
}

TEST(subtitle_overlay_blend, fused_sparse_overlay_matches_two_pass_for_karaoke_progression) {
	auto previous_source = make_frame(24, 8);
	auto previous_composited = previous_source;
	fill_box(previous_composited, 2, 5, 18, 2, 210, 210, 210);
	fill_box(previous_composited, 2, 5, 6, 2, 40, 180, 255);

	auto current_source = make_frame(24, 8);
	auto current_composited = current_source;
	fill_box(current_composited, 2, 5, 18, 2, 210, 210, 210);
	fill_box(current_composited, 2, 5, 11, 2, 40, 180, 255);

	expect_fused_matches_two_pass(
		previous_source,
		previous_composited,
		current_source,
		current_composited,
		4,
		2);
}

TEST(subtitle_overlay_blend, fused_sparse_overlay_matches_two_pass_for_scrolling_banner) {
	auto previous_source = make_frame(28, 8);
	auto previous_composited = previous_source;
	fill_box(previous_composited, 1, 2, 16, 1, 240, 240, 240);
	fill_box(previous_composited, 1, 3, 16, 1, 120, 160, 220);

	auto current_source = make_frame(28, 8);
	auto current_composited = current_source;
	fill_box(current_composited, 5, 2, 16, 1, 240, 240, 240);
	fill_box(current_composited, 5, 3, 16, 1, 120, 160, 220);

	expect_fused_matches_two_pass(
		previous_source,
		previous_composited,
		current_source,
		current_composited,
		4,
		2);
}
