#include <main.h>

#include "../../src/video_color_pick.h"

#include <array>
#include <cstddef>
#include <cstdlib>

namespace {

VideoFrame MakeFrame(int width, int height, size_t padding = 0) {
	VideoFrame frame;
	frame.width = static_cast<size_t>(width);
	frame.height = static_cast<size_t>(height);
	frame.pitch = static_cast<size_t>(width) * 4 + padding;
	frame.flipped = false;
	frame.data.assign(frame.pitch * frame.height, 0);
	return frame;
}

void SetPixel(VideoFrame& frame, int x, int y, unsigned char r, unsigned char g, unsigned char b) {
	auto const row =
		static_cast<size_t>(frame.flipped ? static_cast<int>(frame.height) - 1 - y : y);
	unsigned char *bytes = frame.data.data() + row * frame.pitch + static_cast<size_t>(x) * 4;
	bytes[0] = b;
	bytes[1] = g;
	bytes[2] = r;
	bytes[3] = 255;
}

void FillSolid(
	VideoFrame& frame,
	unsigned char r,
	unsigned char g,
	unsigned char b,
	int left = 0,
	int top = 0,
	int right = -1,
	int bottom = -1) {
	int const width = static_cast<int>(frame.width);
	int const height = static_cast<int>(frame.height);
	if (right < 0)
		right = width - 1;
	if (bottom < 0)
		bottom = height - 1;
	for (int y = top; y <= bottom; ++y)
		for (int x = left; x <= right; ++x)
			SetPixel(frame, x, y, r, g, b);
}

bool ChannelsNear(
	agi::Color const& color,
	unsigned char r,
	unsigned char g,
	unsigned char b,
	int slack) {
	return std::abs(static_cast<int>(color.r) - r) <= slack && std::abs(static_cast<int>(color.g) - g) <= slack && std::abs(static_cast<int>(color.b) - b) <= slack;
}

void ExpectMapped(
	SourceFrameGeometry const& geometry,
	double x,
	double y,
	int expected_x,
	int expected_y) {
	auto const mapped = aegisub::color_pick::MapDisplayPointToStorage(geometry, x, y);
	EXPECT_EQ(expected_x, mapped.first);
	EXPECT_EQ(expected_y, mapped.second);
}

} // namespace

TEST(video_color_pick, returns_empty_result_for_unusable_frames) {
	auto empty = MakeFrame(8, 8);
	empty.data.clear();
	EXPECT_EQ(0, aegisub::color_pick::PickColor(empty, 4, 4).pixels);

	auto short_pitch = MakeFrame(8, 8);
	short_pitch.pitch = 4;
	EXPECT_EQ(0, aegisub::color_pick::PickColor(short_pitch, 4, 4).pixels);

	auto zero_size = MakeFrame(0, 0);
	EXPECT_EQ(0, aegisub::color_pick::PickColor(zero_size, 0, 0).pixels);
}

TEST(video_color_pick, picks_a_stable_median_from_a_noisy_flat_region) {
	auto frame = MakeFrame(64, 64);
	FillSolid(frame, 50, 120, 200);
	// Deterministic salt-and-pepper perturbations scattered over the field.
	for (int i = 0; i < 64; ++i) {
		int const x = (i * 7) % 64;
		int const y = (i * 13) % 64;
		SetPixel(frame, x, y, 50 + (i % 3), 118 + (i % 5), 202 - (i % 3));
	}

	auto const result =
		aegisub::color_pick::PickColor(frame, 32, 32, {});
	ASSERT_FALSE(result.fallback);
	EXPECT_FALSE(result.edge_snapped);
	EXPECT_TRUE(ChannelsNear(result.color, 50, 120, 200, 4));
	EXPECT_GT(result.pixels, 2000);
	EXPECT_FALSE(result.capped);
}

TEST(video_color_pick, snaps_a_seam_click_to_a_stable_block_instead_of_the_blend) {
	auto frame = MakeFrame(96, 48);
	FillSolid(frame, 180, 40, 40, 0, 0, 47);   // red block
	FillSolid(frame, 110, 50, 110, 48, 0, 48); // 50% seam column
	FillSolid(frame, 40, 60, 180, 49, 0, 95);  // blue block

	// Clicked exactly on the blended column; the seed alone would be a mix.
	auto const result = aegisub::color_pick::PickColor(frame, 48, 24, {});
	EXPECT_TRUE(result.edge_snapped);
	EXPECT_FALSE(result.fallback);
	EXPECT_TRUE(ChannelsNear(result.color, 40, 60, 180, 4))
		<< "seam pick must land on a source block, got " << result.color.GetHexFormatted();

	// The same frame picked well inside the red block behaves conventionally.
	auto const inside = aegisub::color_pick::PickColor(frame, 12, 24, {});
	EXPECT_FALSE(inside.edge_snapped);
	EXPECT_TRUE(ChannelsNear(inside.color, 180, 40, 40, 4));
}

TEST(video_color_pick, grows_larger_regions_with_looser_tolerances) {
	auto frame = MakeFrame(64, 64);
	FillSolid(frame, 200, 200, 200);
	FillSolid(frame, 206, 206, 206, 20, 20, 43); // +6 per channel, luma delta 6

	aegisub::color_pick::Options tight;
	tight.luma_tolerance = 4;
	tight.chroma_tolerance = 4;
	aegisub::color_pick::Options loose;
	loose.luma_tolerance = 12;
	loose.chroma_tolerance = 12;

	auto const small_region = aegisub::color_pick::PickColor(frame, 32, 32, tight);
	auto const merged_region = aegisub::color_pick::PickColor(frame, 32, 32, loose);
	EXPECT_FALSE(small_region.fallback);
	EXPECT_FALSE(merged_region.fallback);
	EXPECT_LT(small_region.pixels, merged_region.pixels);
	EXPECT_EQ(4096, merged_region.pixels);
}

TEST(video_color_pick, reports_lower_confidence_at_low_contrast_edges) {
	aegisub::color_pick::Options tight;
	tight.luma_tolerance = 4;
	tight.chroma_tolerance = 6;

	auto faint_edge_frame = MakeFrame(48, 48);
	FillSolid(faint_edge_frame, 100, 100, 100);
	FillSolid(faint_edge_frame, 110, 110, 110, 24, 0, 47);

	auto strong_edge_frame = MakeFrame(48, 48);
	FillSolid(strong_edge_frame, 100, 100, 100);
	FillSolid(strong_edge_frame, 240, 240, 240, 24, 0, 47);

	auto const faint = aegisub::color_pick::PickColor(faint_edge_frame, 8, 24, tight);
	auto const strong = aegisub::color_pick::PickColor(strong_edge_frame, 8, 24, tight);
	EXPECT_GT(faint.confidence, 0.0);
	EXPECT_LT(faint.confidence, strong.confidence);
	EXPECT_TRUE(ChannelsNear(faint.color, 100, 100, 100, 2));
	EXPECT_TRUE(ChannelsNear(strong.color, 100, 100, 100, 2));
}

TEST(video_color_pick, honours_flipped_rows_and_stride_padding) {
	// Wide alternating bands keep the seed window inside one colour block;
	// any row-addressing mistake (flip or stride) lands on the other colour.
	auto color_for_band = [](int y) {
		return (y / 4) % 2 == 0 ? std::array<unsigned char, 3>{200, 200, 200}
								: std::array<unsigned char, 3>{40, 40, 40};
	};
	auto build = [&](size_t padding, bool flipped) {
		auto frame = MakeFrame(48, 32, padding);
		frame.flipped = flipped;
		for (int y = 0; y < 32; ++y)
			for (int x = 0; x < 48; ++x) {
				auto const rgb = color_for_band(y);
				SetPixel(frame, x, y, rgb[0], rgb[1], rgb[2]);
			}
		return frame;
	};

	VideoFrame straight = build(0, false);
	VideoFrame padded_flipped = build(16, true);

	aegisub::color_pick::Options options;
	options.luma_tolerance = 12;
	options.chroma_tolerance = 12;

	auto const a = aegisub::color_pick::PickColor(straight, 24, 10, options); // band 2: bright
	EXPECT_TRUE(ChannelsNear(a.color, 200, 200, 200, 2)) << a.color.GetHexFormatted();
	EXPECT_FALSE(a.fallback);

	auto const b = aegisub::color_pick::PickColor(padded_flipped, 24, 10, options);
	EXPECT_EQ(a.color.r, b.color.r);
	EXPECT_EQ(a.color.g, b.color.g);
	EXPECT_EQ(a.color.b, b.color.b);
	EXPECT_EQ(a.pixels, b.pixels);

	auto const c = aegisub::color_pick::PickColor(padded_flipped, 10, 30, options); // band 7: dark
	EXPECT_TRUE(ChannelsNear(c.color, 40, 40, 40, 2)) << c.color.GetHexFormatted();
}

TEST(video_color_pick, falls_back_for_out_of_range_points) {
	auto frame = MakeFrame(32, 32);
	FillSolid(frame, 90, 30, 170);

	auto const below = aegisub::color_pick::PickColor(frame, -40000, 16, {});
	EXPECT_TRUE(below.fallback);
	EXPECT_TRUE(ChannelsNear(below.color, 90, 30, 170, 0));

	auto const beyond = aegisub::color_pick::PickColor(frame, 16, 40000, {});
	EXPECT_TRUE(beyond.fallback);
	EXPECT_TRUE(ChannelsNear(beyond.color, 90, 30, 170, 0));
	EXPECT_GT(below.pixels, 0);

	// The clamped-to-edge window loses its outer column/row, and the reported
	// bbox is the window actually sampled rather than the unclipped 3x3.
	EXPECT_EQ(2, below.bbox_w);
	EXPECT_EQ(3, below.bbox_h);
	EXPECT_EQ(below.pixels, below.bbox_w * below.bbox_h);
	EXPECT_EQ(3, beyond.bbox_w);
	EXPECT_EQ(2, beyond.bbox_h);
	EXPECT_EQ(beyond.pixels, beyond.bbox_w * beyond.bbox_h);
}

TEST(video_color_pick, degenerate_region_fallback_reports_the_window_bbox) {
	auto frame = MakeFrame(32, 32);
	FillSolid(frame, 100, 100, 100);
	// An alternating light/dark ring around the click point leaves the seed
	// window's per-channel median matching only the centre pixel, so the
	// flood fill degenerates and the pick falls back to the local median.
	for (int dy = -1; dy <= 1; ++dy)
		for (int dx = -1; dx <= 1; ++dx) {
			if (dx == 0 && dy == 0)
				continue;
			auto const value = static_cast<unsigned char>((dx + dy + 2) % 2 * 255);
			SetPixel(frame, 16 + dx, 16 + dy, value, value, value);
		}

	auto const result = aegisub::color_pick::PickColor(frame, 16, 16, {});
	EXPECT_TRUE(result.fallback);
	EXPECT_TRUE(ChannelsNear(result.color, 100, 100, 100, 0));
	EXPECT_EQ(9, result.pixels);
	EXPECT_EQ(3, result.bbox_w);
	EXPECT_EQ(3, result.bbox_h);
	EXPECT_EQ(result.pixels, result.bbox_w * result.bbox_h);
}

TEST(video_color_pick, stops_growth_at_the_pixel_and_extent_caps) {
	auto frame = MakeFrame(128, 128);
	FillSolid(frame, 180, 150, 120);

	aegisub::color_pick::Options pixel_cap;
	pixel_cap.max_region_pixels = 500;
	auto const by_pixels = aegisub::color_pick::PickColor(frame, 64, 64, pixel_cap);
	EXPECT_TRUE(by_pixels.capped);
	EXPECT_EQ(500, by_pixels.pixels);

	aegisub::color_pick::Options extent_cap;
	extent_cap.max_region_pixels = 32768;
	extent_cap.max_region_extent = 15;
	auto const by_extent = aegisub::color_pick::PickColor(frame, 64, 64, extent_cap);
	EXPECT_TRUE(by_extent.capped);
	EXPECT_LE(by_extent.bbox_w, 16 + 2);
	EXPECT_LE(by_extent.bbox_h, 16 + 2);
}

TEST(video_color_pick, maps_display_points_to_storage_without_crop) {
	auto const geometry = MakeDefaultSourceFrameGeometry(640, 480);
	ExpectMapped(geometry, 100.0, 200.0, 100, 200);
	ExpectMapped(geometry, -20.0, -20.0, 0, 0);
	// Clamps to the visible extent, which here equals the storage extent.
	ExpectMapped(geometry, 5000.0, -3.0, 639, 0);
	ExpectMapped(geometry, 639.4, 479.4, 639, 479);
}

TEST(video_color_pick, maps_display_points_through_a_crop_offset) {
	auto geometry = MakeDefaultSourceFrameGeometry(720, 480);
	geometry.visible_rect = {.x = 8, .y = 4, .width = 704, .height = 472};

	ExpectMapped(geometry, 0.0, 0.0, 8, 4);
	ExpectMapped(geometry, 703.0, 471.0, 711, 475);
	// Clamps into the visible rect, never into the cropped-away margins.
	ExpectMapped(geometry, 2000.0, -10.0, 711, 4);
}

TEST(video_color_pick, maps_display_points_through_rotation_and_vflip) {
	// 90° metadata turn: storage 720x480 becomes 480x720 after baking and the
	// visible rect {8,0,704,480} becomes {0,8,480,704}.
	auto rotated = MakeDefaultSourceFrameGeometry(720, 480);
	rotated.visible_rect = {.x = 8, .y = 0, .width = 704, .height = 480};
	rotated.rotation = 90;
	ExpectMapped(rotated, 0.0, 0.0, 0, 8);
	ExpectMapped(rotated, 479.0, 703.0, 479, 711);

	// Display vflip moves a top band of the visible rect to the bottom.
	auto vflipped = MakeDefaultSourceFrameGeometry(100, 100);
	vflipped.visible_rect = {.x = 0, .y = 0, .width = 100, .height = 40};
	vflipped.display_vflip = true;
	ExpectMapped(vflipped, 0.0, 0.0, 0, 60);
	ExpectMapped(vflipped, 99.0, 39.0, 99, 99);
}

TEST(video_color_pick, rejects_degenerate_geometry_for_mapping) {
	SourceFrameGeometry geometry;
	auto const mapped = aegisub::color_pick::MapDisplayPointToStorage(geometry, 0.0, 0.0);
	EXPECT_EQ(-1, mapped.first);
	EXPECT_EQ(-1, mapped.second);
}

TEST(video_color_pick, extracts_a_centered_zoom_region_with_pitch_padding) {
	auto frame = MakeFrame(16, 16, 12);
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x)
			SetPixel(frame, x, y, static_cast<unsigned char>(x * 7), static_cast<unsigned char>(y * 11), 3);

	auto const region = aegisub::color_pick::ExtractZoomRegion(frame, 8, 8, 1);
	ASSERT_EQ(9u, region.size());
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			auto const& cell = region[static_cast<size_t>(dy + 1) * 3 + (dx + 1)];
			EXPECT_TRUE(ChannelsNear(cell, (8 + dx) * 7, (8 + dy) * 11, 3, 0));
		}
	}
	EXPECT_TRUE(ChannelsNear(region[4], 8 * 7, 8 * 11, 3, 0));
}

TEST(video_color_pick, zoom_region_reads_a_flipped_frame_in_display_order) {
	auto frame = MakeFrame(5, 5);
	frame.flipped = true;
	// Colour the display rows the radius-1 window at the centre covers.
	FillSolid(frame, 200, 20, 20, 0, 1, 4, 1); // display row 1 red
	FillSolid(frame, 20, 20, 200, 0, 3, 4, 3); // display row 3 blue

	auto const region = aegisub::color_pick::ExtractZoomRegion(frame, 2, 2, 1);
	ASSERT_EQ(9u, region.size());
	for (int dx = 0; dx < 3; ++dx) {
		EXPECT_TRUE(ChannelsNear(region[dx], 200, 20, 20, 0));
		EXPECT_TRUE(ChannelsNear(region[6 + dx], 20, 20, 200, 0));
	}
}

TEST(video_color_pick, zoom_region_clamps_at_frame_edges) {
	auto frame = MakeFrame(8, 8);
	FillSolid(frame, 255, 0, 0, 0, 0, 0, 7); // column 0 red
	FillSolid(frame, 0, 0, 255, 1, 0, 7, 7); // the rest blue

	// Centre on column 0: the dx=-1 cells clamp onto it, and the centre
	// column is that red column itself.
	auto const region = aegisub::color_pick::ExtractZoomRegion(frame, 0, 4, 1);
	ASSERT_EQ(9u, region.size());
	for (int dy = 0; dy < 3; ++dy) {
		EXPECT_TRUE(ChannelsNear(region[dy * 3], 255, 0, 0, 0));
		EXPECT_TRUE(ChannelsNear(region[dy * 3 + 1], 255, 0, 0, 0));
		EXPECT_TRUE(ChannelsNear(region[dy * 3 + 2], 0, 0, 255, 0));
	}
}

TEST(video_color_pick, zoom_region_rejects_unusable_frames_and_radius) {
	auto empty = MakeFrame(8, 8);
	empty.data.clear();
	EXPECT_TRUE(aegisub::color_pick::ExtractZoomRegion(empty, 4, 4, 2).empty());

	auto short_pitch = MakeFrame(8, 8);
	short_pitch.pitch = 4;
	EXPECT_TRUE(aegisub::color_pick::ExtractZoomRegion(short_pitch, 4, 4, 2).empty());

	auto truncated = MakeFrame(8, 8);
	truncated.data.resize(truncated.data.size() / 2);
	EXPECT_TRUE(aegisub::color_pick::ExtractZoomRegion(truncated, 4, 4, 2).empty());

	auto frame = MakeFrame(8, 8);
	FillSolid(frame, 9, 9, 9);
	EXPECT_TRUE(aegisub::color_pick::ExtractZoomRegion(frame, 4, 4, -1).empty());
}
