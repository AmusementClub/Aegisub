#include <main.h>

#include "../../src/motion_track/gray_convert.h"
#include "../../src/motion_track/ncc.h"
#include "../../src/motion_track/synthetic_frame_reader.h"

#ifdef AEGISUB_WITH_HIGHWAY
#include "../../src/simd/motion_track_ncc_simd.h"
#endif

#include <cstdint>
#include <vector>

namespace {
using namespace aegisub::motion_track;

// Small textured scene with an object that translates at integer velocity.
SyntheticTranslationScene MakeScene(double vx = 0.0, double vy = 0.0) {
	SyntheticTranslationScene::Config config;
	config.width = 96;
	config.height = 64;
	config.frame_count = 8;
	config.object_at_frame0 = RoiRect{20, 12, 24, 16};
	config.velocity_x = vx;
	config.velocity_y = vy;
	return SyntheticTranslationScene(config);
}

GrayPatch FetchScene(SyntheticTranslationScene const& scene, int frame,
                     RoiRect roi) {
	SyntheticFrameReader reader(scene);
	GrayPatch patch;
	EXPECT_EQ(FrameReadStatus::Ok, reader.FetchGray(frame, roi, patch).status);
	return patch;
}

BgraView MakeBgra(int width, int height, int stride, bool flipped) {
	static std::vector<std::uint8_t> buffer;
	buffer.assign(size_t(stride) * size_t(height), 0);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			auto* px = buffer.data() + size_t(y) * stride + size_t(x) * 4;
			px[0] = static_cast<std::uint8_t>(x * 3 + 1);     // B
			px[1] = static_cast<std::uint8_t>(y * 5 + 2);     // G
			px[2] = static_cast<std::uint8_t>(x + y * 7 + 3); // R
			px[3] = 0xEE;                                     // ignored
		}
	}
	return BgraView{buffer.data(), stride, width, height, flipped};
}
}

TEST(motion_track_gray_convert, applies_bt601_integer_formula_exactly) {
	BgraView view = MakeBgra(4, 2, 4 * 5, false); // padded stride
	GrayPatch out;
	ASSERT_TRUE(BgraToGray(view, out));

	ASSERT_EQ(4, out.width);
	ASSERT_EQ(2, out.height);
	EXPECT_EQ(4, out.stride);
	EXPECT_EQ(0, out.origin_x);
	EXPECT_EQ(0, out.origin_y);

	for (int y = 0; y < 2; ++y) {
		for (int x = 0; x < 4; ++x) {
			int const b = (x * 3 + 1) & 0xFF;
			int const g = (y * 5 + 2) & 0xFF;
			int const r = (x + y * 7 + 3) & 0xFF;
			int const expected = (77 * r + 150 * g + 29 * b) >> 8;
			EXPECT_EQ(expected, out.gray[size_t(y) * 4 + size_t(x)])
			    << "at " << x << "," << y;
		}
	}
}

TEST(motion_track_gray_convert, flipped_reads_rows_bottom_up) {
	BgraView normal = MakeBgra(3, 3, 3 * 4, false);
	BgraView flipped = MakeBgra(3, 3, 3 * 4, true);
	GrayPatch a;
	GrayPatch b;
	ASSERT_TRUE(BgraToGray(normal, a));
	ASSERT_TRUE(BgraToGray(flipped, b));
	for (int y = 0; y < 3; ++y)
		for (int x = 0; x < 3; ++x)
			EXPECT_EQ(a.gray[size_t(y) * 3 + size_t(x)],
			          b.gray[size_t(2 - y) * 3 + size_t(x)]);
}

TEST(motion_track_gray_convert, logical_row_to_physical_row_maps_row_order) {
	EXPECT_EQ(0, LogicalRowToPhysicalRow(0, 4, false));
	EXPECT_EQ(3, LogicalRowToPhysicalRow(3, 4, false));
	EXPECT_EQ(3, LogicalRowToPhysicalRow(0, 4, true));
	EXPECT_EQ(0, LogicalRowToPhysicalRow(3, 4, true));
	EXPECT_EQ(2, LogicalRowToPhysicalRow(1, 4, true));
	EXPECT_EQ(1, LogicalRowToPhysicalRow(2, 4, true));
	EXPECT_EQ(1, LogicalRowToPhysicalRow(1, 3, true)); // odd height middle row
}

TEST(motion_track_gray_convert, rejects_degenerate_views) {
	GrayPatch out;
	EXPECT_FALSE(BgraToGray(BgraView{}, out));
	BgraView bad_stride = MakeBgra(4, 2, 4 * 5, false);
	bad_stride.stride = 10; // < width*4
	EXPECT_FALSE(BgraToGray(bad_stride, out));
}

TEST(motion_track_ncc, perfect_alignment_scores_one) {
	auto scene = MakeScene();
	GrayPatch templ = FetchScene(scene, 0, RoiRect{20, 12, 24, 16});
	GrayPatch image = FetchScene(scene, 0, RoiRect{10, 2, 64, 48});

	double ncc = 0.0;
	ASSERT_TRUE(ZeroMeanNccScalar(
		templ.View(), image.View(), 10, 10, ncc));
	EXPECT_NEAR(1.0, ncc, 1e-9);

	auto best = FindBestNccScalar(templ.View(), image.View(), 0, 0, 20, 20);
	ASSERT_TRUE(best.found);
	EXPECT_EQ(10, best.offset_x);
	EXPECT_EQ(10, best.offset_y);
	EXPECT_NEAR(1.0, best.ncc, 1e-9);
}

TEST(motion_track_ncc, flat_sides_report_not_found) {
	GrayPatch flat{0, 0, 0, 8, 8, 8, std::vector<std::uint8_t>(64, 42)};
	GrayView flat_view = flat.View();
	double ncc = 0.0;
	// Flat template: denominator vanishes regardless of the image.
	GrayPatch textured = FetchScene(MakeScene(), 0, RoiRect{0, 0, 32, 32});
	EXPECT_FALSE(ZeroMeanNccScalar(flat_view, textured.View(), 0, 0, ncc));

	auto best = FindBestNccScalar(
		flat_view, textured.View(), 0, 0, 8, 8);
	EXPECT_FALSE(best.found);
}

TEST(motion_track_ncc, parabolic_subpixel_symmetry_and_local_maximum_gate) {
	EXPECT_NEAR(0.0, ParabolicSubpixel(0.5, 1.0, 0.5), 1e-12);
	// A center that is not a local maximum models no peak: the parabola
	// through it opens upward and its vertex points away from the higher
	// neighbour, so the refinement holds the integer peak instead of
	// steering (these triples used to return that upward parabola's
	// vertex, clamped to ±0.75).
	EXPECT_NEAR(0.0, ParabolicSubpixel(0.0, 0.4, 0.6), 1e-12);
	EXPECT_NEAR(0.0, ParabolicSubpixel(0.6, 0.4, 0.0), 1e-12);
	// In-range asymmetric peak keeps its raw value.
	EXPECT_NEAR(-0.38888888888888889,
	            ParabolicSubpixel(0.9, 1.0, 0.2), 1e-12);
	// Vanishing curvature yields zero.
	EXPECT_EQ(0.0, ParabolicSubpixel(1.0, 1.0, 1.0));
}

TEST(motion_track_synthetic_reader, object_center_follows_analytic_path) {
	auto scene = MakeScene(5.0, -2.0);
	EXPECT_DOUBLE_EQ(20.0 + 11.5, scene.ObjectCenterX(0));
	EXPECT_DOUBLE_EQ(20.0 + 5.0 * 3 + 11.5, scene.ObjectCenterX(3));
	EXPECT_DOUBLE_EQ(12.0 + 7.5 - 2.0 * 2, scene.ObjectCenterY(2));
}

TEST(motion_track_synthetic_reader, fetch_matches_scene_pixels_and_fill_rule) {
	auto scene = MakeScene(3.0, 0.0);
	SyntheticFrameReader reader(scene);

	// Fully in-bounds crop matches Pixel() exactly.
	GrayPatch patch;
	ASSERT_EQ(FrameReadStatus::Ok,
	          reader.FetchGray(2, RoiRect{30, 5, 12, 10}, patch).status);
	ASSERT_EQ(2, patch.frame);
	ASSERT_EQ(30, patch.origin_x);
	ASSERT_EQ(5, patch.origin_y);
	ASSERT_EQ(12, patch.stride);
	for (int y = 0; y < 10; ++y)
		for (int x = 0; x < 12; ++x)
			EXPECT_EQ(scene.Pixel(30 + x, 5 + y, 2),
			          patch.gray[size_t(y) * 12 + size_t(x)]);

	// Partially out of bounds: border pixels use the intersection mean.
	RoiRect clipped{90, 0, 12, 8}; // 6 columns inside
	ASSERT_EQ(FrameReadStatus::Ok, reader.FetchGray(1, clipped, patch).status);
	ASSERT_EQ(90, patch.origin_x);
	long long sum = 0;
	long long count = 0;
	for (int y = 0; y < 8; ++y)
		for (int x = 90; x < 96; ++x) {
			sum += scene.Pixel(x, y, 1);
			++count;
		}
	auto const expected_fill =
		static_cast<std::uint8_t>((sum + count / 2) / count);
	// First column is in-bounds: exact scene pixel, not fill.
	EXPECT_EQ(scene.Pixel(90, 0, 1), patch.gray[0]);
	// Last column is out of bounds: intersection-mean fill.
	EXPECT_EQ(expected_fill, patch.gray[11]);

	// Fully outside: neutral 128.
	ASSERT_EQ(FrameReadStatus::Ok,
	          reader.FetchGray(1, RoiRect{-20, -20, 8, 8}, patch).status);
	EXPECT_EQ(128, patch.gray[0]);

	// Invalid frame.
	EXPECT_EQ(FrameReadStatus::FrameUnavailable,
	          reader.FetchGray(8, RoiRect{0, 0, 4, 4}, patch).status);
	EXPECT_EQ(FrameReadStatus::FrameUnavailable,
	          reader.FetchGray(-1, RoiRect{0, 0, 4, 4}, patch).status);
}

TEST(motion_track_ncc, tracks_integer_translation_across_frames) {
	auto scene = MakeScene(5.0, 0.0);
	GrayPatch templ = FetchScene(scene, 0, RoiRect{20, 12, 24, 16});

	for (int frame = 1; frame < 4; ++frame) {
		int const expected_dx = 5 * frame;
		// Search window around the predicted position.
		GrayPatch image = FetchScene(
			scene, frame,
			RoiRect{20 + expected_dx - 6, 6, 24 + 12, 16 + 12});
		auto best = FindBestNccScalar(
			templ.View(), image.View(), 0, 0, 12, 12);
		ASSERT_TRUE(best.found) << "frame " << frame;
		EXPECT_EQ(6, best.offset_x) << "frame " << frame;
		EXPECT_EQ(6, best.offset_y) << "frame " << frame;
		EXPECT_NEAR(1.0, best.ncc, 1e-9);
	}
}

#ifdef AEGISUB_WITH_HIGHWAY

namespace {
void ExpectScalarSimdParity(GrayView templ, GrayView image,
                            int min_ox, int min_oy, int max_ox, int max_oy) {
	auto const scalar = FindBestNccScalar(
		templ, image, min_ox, min_oy, max_ox, max_oy);
	auto const simd = simd::FindBestNccSimd(
		templ, image, min_ox, min_oy, max_ox, max_oy);
	ASSERT_EQ(scalar.found, simd.found);
	if (!scalar.found) return;
	// Integer accumulators are exact on both paths: identical peak offset and
	// bit-stable double division.
	EXPECT_EQ(scalar.offset_x, simd.offset_x);
	EXPECT_EQ(scalar.offset_y, simd.offset_y);
	EXPECT_NEAR(scalar.ncc, simd.ncc, 1e-6);
}
}

TEST(motion_track_ncc_simd, matches_scalar_offsets_and_ncc) {
	auto scene = MakeScene(2.0, 1.0);
	GrayPatch templ = FetchScene(scene, 0, RoiRect{20, 12, 24, 16});
	GrayPatch image = FetchScene(scene, 4, RoiRect{8, 4, 72, 48});
	ExpectScalarSimdParity(templ.View(), image.View(), 0, 0, 40, 24);
}

TEST(motion_track_ncc_simd, parity_holds_across_sizes_and_flat_windows) {
	// Odd width exercises the vector tail path.
	auto scene = MakeScene(-1.0, 2.0);
	GrayPatch templ_odd = FetchScene(scene, 1, RoiRect{30, 20, 17, 11});
	GrayPatch image_odd = FetchScene(scene, 3, RoiRect{10, 8, 64, 40});
	ExpectScalarSimdParity(templ_odd.View(), image_odd.View(), 0, 0, 32, 20);

	// Flat template: both paths must agree that nothing was found.
	GrayPatch flat{0, 0, 0, 9, 7, 9,
	               std::vector<std::uint8_t>(size_t(63), 77)};
	ExpectScalarSimdParity(flat.View(), image_odd.View(), 0, 0, 16, 16);

	// Single-column template (width < any vector).
	GrayPatch thin = FetchScene(scene, 0, RoiRect{44, 10, 3, 13});
	ExpectScalarSimdParity(thin.View(), image_odd.View(), 0, 0, 24, 12);
}

#endif // AEGISUB_WITH_HIGHWAY
