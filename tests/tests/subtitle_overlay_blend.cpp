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
