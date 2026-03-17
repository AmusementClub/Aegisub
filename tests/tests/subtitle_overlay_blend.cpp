#include <main.h>

#include "../../src/subtitle_overlay_blend.h"

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

	SubtitleOverlayStorage current;
	current.Reset(8, 4, false);
	current.has_visible_content = true;
	current.pixels[64] = 2;
	current.pixels[67] = 255;

	ASSERT_TRUE(BuildDirtyTileRectsForOverlay(&previous, current, 4, 2));
	ASSERT_FALSE(current.dirty_rects.empty());
	for (auto const& rect : current.dirty_rects) {
		EXPECT_EQ(0, rect.x);
		EXPECT_EQ(4, rect.width);
		EXPECT_GE(rect.y, 0);
		EXPECT_LE(rect.y + rect.height, 4);
	}
}
