#include <main.h>

#include "../../src/motion_track/planar_backend.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {
using namespace aegisub::motion_track;

constexpr RoiRect kPlane{.x = 64, .y = 48, .w = 64, .h = 48};
constexpr double kCenterX = 95.5;
constexpr double kCenterY = 71.5;

TrackTransform Pose(TrackModel model, int frame) {
	TrackTransform pose;
	pose.matrix = {1 + 0.004 * frame, 0.003 * frame, 0,
				   -0.002 * frame, 1 - 0.003 * frame, 0,
				   model == TrackModel::Homography ? 0.00008 * frame : 0,
				   model == TrackModel::Homography ? -0.00005 * frame : 0, 1};
	return pose;
}

// A continuous nonperiodic mixture of spatial frequencies provides gradients
// in both directions. Render the image by inverse projection independently
// of the tracker's forward warp and derivative implementation.
GrayPatch Render(TrackModel model, int frame, RoiRect crop, bool occluded = false) {
	GrayPatch patch;
	patch.origin_x = crop.x;
	patch.origin_y = crop.y;
	patch.width = patch.stride = crop.w;
	patch.height = crop.h;
	patch.gray.resize(static_cast<size_t>(crop.w) * crop.h);
	auto const& m = Pose(model, frame).matrix;
	for (int y = 0; y < crop.h; ++y)
		for (int x = 0; x < crop.w; ++x) {
			double const dx = crop.x + x - kCenterX - 0.8 * frame;
			double const dy = crop.y + y - kCenterY + 0.4 * frame;
			double const a = m[0] - dx * m[6], b = m[1] - dx * m[7];
			double const c = m[3] - dy * m[6], d = m[4] - dy * m[7];
			double const determinant = a * d - b * c;
			double const u = (d * dx - b * dy) / determinant;
			double const v = (a * dy - c * dx) / determinant;
			double const texture = 128 + 30 * std::sin(u * 0.19 + v * 0.07) +
								   24 * std::cos(v * 0.27 - u * 0.09) +
								   20 * std::sin(u * 0.41 + 1.7) * std::cos(v * 0.33 + 0.4);
			patch.gray[static_cast<size_t>(y) * crop.w + x] = static_cast<std::uint8_t>(
				std::clamp(occluded && u > 23 ? 210.0 : std::round(texture), 0.0, 255.0));
		}
	return patch;
}

TrackerSeed Seed(TrackModel model, GrayPatch const& patch) {
	return {.model = model, .roi = kPlane, .template_gray = patch.View(), .seed_frame = 0};
}

TrackStepRequest Request(GrayPatch const& patch, int frame) {
	TrackStepRequest request;
	request.frame = frame;
	request.image = patch.View();
	request.image_origin_x = patch.origin_x;
	request.image_origin_y = patch.origin_y;
	request.search_center_x = kCenterX;
	request.search_center_y = kCenterY;
	return request;
}

void ExpectCorners(TrackStepResult const& result, TrackTransform const& expected,
				   double expected_x, double expected_y) {
	for (double y : {-20.0, 20.0})
		for (double x : {-28.0, 28.0}) {
			auto const& actual = result.transform.matrix;
			auto const& truth = expected.matrix;
			double const aw = actual[6] * x + actual[7] * y + actual[8];
			double const tw = truth[6] * x + truth[7] * y + truth[8];
			EXPECT_NEAR(expected_x + (truth[0] * x + truth[1] * y) / tw,
						result.candidate_center_x + (actual[0] * x + actual[1] * y) / aw, 0.3);
			EXPECT_NEAR(expected_y + (truth[3] * x + truth[4] * y) / tw,
						result.candidate_center_y + (actual[3] * x + actual[4] * y) / aw, 0.3);
		}
}

void TrackSequence(TrackModel model) {
	PlanarTrackerBackend backend(model);
	auto seed = Render(model, 0, kPlane);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(Seed(model, seed)));
	TrackTransform prior;
	double cx = kCenterX, cy = kCenterY;
	for (int frame = 1; frame <= 20; ++frame) {
		auto patch = Render(model, frame, {.x = 32, .y = 16, .w = 144, .h = 112});
		auto request = Request(patch, frame);
		request.search_center_x = cx;
		request.search_center_y = cy;
		request.init_transform = prior;
		auto result = backend.Step(request);
		ASSERT_EQ(TrackStatus::Ok, result.status) << "frame " << frame << " failure " << static_cast<int>(result.failure);
		ExpectCorners(result, Pose(model, frame), kCenterX + 0.8 * frame, kCenterY - 0.4 * frame);
		EXPECT_NEAR(kCenterX + 0.8 * frame, result.candidate_center_x, 0.15);
		EXPECT_NEAR(kCenterY - 0.4 * frame, result.candidate_center_y, 0.15);
		if (model == TrackModel::Affine) {
			EXPECT_DOUBLE_EQ(0, result.transform.matrix[6]);
			EXPECT_DOUBLE_EQ(0, result.transform.matrix[7]);
		}
		prior = result.transform;
		cx = result.candidate_center_x;
		cy = result.candidate_center_y;
	}
}
} // namespace

TEST(motion_track_planar, affine_tracks_nonuniform_scale_and_shear) {
	TrackSequence(TrackModel::Affine);
}

TEST(motion_track_planar, homography_tracks_both_perspective_axes) {
	TrackSequence(TrackModel::Homography);
}

TEST(motion_track_planar, rejects_wrong_model_and_uninitialized_step) {
	PlanarTrackerBackend backend(TrackModel::Affine);
	auto patch = Render(TrackModel::Affine, 0, kPlane);
	EXPECT_EQ(TrackStatus::InvalidInput, backend.Step(Request(patch, 1)).status);
	EXPECT_EQ(TrackStatus::Unsupported, backend.Reset(Seed(TrackModel::Homography, patch)));
	EXPECT_EQ(TrackStatus::InvalidInput, backend.Reset({TrackModel::Affine, kPlane, {}, 0}));
}

TEST(motion_track_planar, rejects_flat_template_and_fully_occluded_frame) {
	PlanarTrackerBackend backend(TrackModel::Homography);
	auto seed = Render(TrackModel::Homography, 0, kPlane);
	auto patch = Render(TrackModel::Homography, 1, {.x = 32, .y = 16, .w = 144, .h = 112});
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(Seed(TrackModel::Homography, seed)));
	std::ranges::fill(patch.gray, 90);
	auto lost = backend.Step(Request(patch, 1));
	EXPECT_EQ(TrackStatus::Failed, lost.status);
	EXPECT_EQ(TrackFailureReason::NccLow, lost.failure);
	std::ranges::fill(seed.gray, 90);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(Seed(TrackModel::Homography, seed)));
	auto flat = backend.Step(Request(patch, 1));
	EXPECT_EQ(TrackStatus::Failed, flat.status);
	EXPECT_EQ(TrackFailureReason::NccLow, flat.failure);
}

TEST(motion_track_planar, partial_occlusion_retains_visible_plane) {
	PlanarTrackerBackend backend(TrackModel::Homography);
	auto seed = Render(TrackModel::Homography, 0, kPlane);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(Seed(TrackModel::Homography, seed)));
	auto patch = Render(TrackModel::Homography, 1, {.x = 32, .y = 16, .w = 144, .h = 112}, true);
	auto result = backend.Step(Request(patch, 1));
	ASSERT_EQ(TrackStatus::Ok, result.status) << static_cast<int>(result.failure);
	ExpectCorners(result, Pose(TrackModel::Homography, 1), kCenterX + 0.8, kCenterY - 0.4);
}

TEST(motion_track_planar, rejects_offscreen_and_excessive_corner_motion) {
	PlanarTrackerConfig config;
	config.max_corner_step = 0.1;
	PlanarTrackerBackend backend(TrackModel::Affine, config);
	auto seed = Render(TrackModel::Affine, 0, kPlane);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(Seed(TrackModel::Affine, seed)));
	auto patch = Render(TrackModel::Affine, 1, {.x = 32, .y = 16, .w = 144, .h = 112});
	auto request = Request(patch, 1);
	auto jump = backend.Step(request);
	EXPECT_EQ(TrackStatus::Failed, jump.status);
	EXPECT_EQ(TrackFailureReason::JumpTooLarge, jump.failure);
	request.search_center_x = -100;
	auto offscreen = backend.Step(request);
	EXPECT_EQ(TrackStatus::Failed, offscreen.status);
	EXPECT_EQ(TrackFailureReason::Offscreen, offscreen.failure);
	request = Request(patch, 1);
	request.init_transform.matrix[6] = std::numeric_limits<double>::quiet_NaN();
	auto invalid = backend.Step(request);
	EXPECT_EQ(TrackStatus::Failed, invalid.status);
	EXPECT_EQ(TrackFailureReason::JumpTooLarge, invalid.failure);
}
