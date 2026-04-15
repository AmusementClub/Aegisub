#include <main.h>

#include "../../src/subtitle_overlay_blend.h"

namespace {
VideoFrame make_frame(int width, int height) {
	VideoFrame frame;
	frame.width = static_cast<size_t>(width);
	frame.height = static_cast<size_t>(height);
	frame.pitch = static_cast<size_t>(width) * 4;
	frame.flipped = false;
	frame.data.assign(frame.pitch * frame.height, 0);
	return frame;
}

size_t pixel_offset(VideoFrame const& frame, int x, int y) {
	return static_cast<size_t>(y) * frame.pitch + static_cast<size_t>(x) * 4;
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

TEST(subtitle_overlay_blend, opaque_replace_overlay_copies_patch_to_target_rect) {
	auto frame = make_frame(3, 2);

	SubtitleOverlayStorage storage;
	storage.Reset(1, 1, false);
	storage.pixels[0] = 1;
	storage.pixels[1] = 2;
	storage.pixels[2] = 3;
	storage.pixels[3] = 4;
	storage.has_visible_content = true;

	auto overlay = storage.MakeView(false);
	overlay.canvas_width = 3;
	overlay.canvas_height = 2;
	overlay.target_x = 2;
	overlay.target_y = 1;
	overlay.composition_mode = SubtitleOverlayCompositionMode::OpaqueReplace;

	CompositeOpaqueBgraOverlayOntoVideoFrame(frame, overlay);

	auto const offset = pixel_offset(frame, 2, 1);
	EXPECT_EQ(1, frame.data[offset + 0]);
	EXPECT_EQ(2, frame.data[offset + 1]);
	EXPECT_EQ(3, frame.data[offset + 2]);
	EXPECT_EQ(4, frame.data[offset + 3]);
}

TEST(subtitle_overlay_blend, premultiplied_overlay_composites_onto_video_frame) {
	auto frame = make_frame(2, 1);
	frame.data[0] = 100;
	frame.data[1] = 50;
	frame.data[2] = 0;
	frame.data[3] = 0;
	frame.data[4] = 7;
	frame.data[5] = 8;
	frame.data[6] = 9;
	frame.data[7] = 0;

	SubtitleOverlayStorage storage;
	storage.Reset(1, 1, false);
	storage.pixels[0] = 10;
	storage.pixels[1] = 20;
	storage.pixels[2] = 30;
	storage.pixels[3] = 128;
	storage.has_visible_content = true;

	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 2;
	overlay.canvas_height = 1;
	overlay.target_x = 0;
	overlay.target_y = 0;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	CompositePremultipliedBgraOverlayOntoVideoFrame(frame, overlay);

	EXPECT_EQ(59, frame.data[0]);
	EXPECT_EQ(44, frame.data[1]);
	EXPECT_EQ(30, frame.data[2]);
	EXPECT_EQ(0, frame.data[3]);
	EXPECT_EQ(7, frame.data[4]);
	EXPECT_EQ(8, frame.data[5]);
	EXPECT_EQ(9, frame.data[6]);
	EXPECT_EQ(0, frame.data[7]);
}
