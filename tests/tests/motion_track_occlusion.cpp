// Occlusion-robust acceptance, NCC tie-break and exact-match subpixel skip.
// The robust selection rule and the anti-drift details are adapted from
// croni1012/Aegisub src/typesetting_auto_motion.cpp (ISC license); the
// scenes and the assertions are ours.

#include <main.h>

#include "../../src/motion_track/ncc.h"
#include "../../src/motion_track/synthetic_frame_reader.h"
#include "../../src/motion_track/translation_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
using namespace aegisub::motion_track;

RoiRect const kObject{20, 14, 40, 28};

double ObjectCenterX(int frame) {
	return kObject.x + (kObject.w - 1) / 2.0 + frame;
}
double ObjectCenterY() { return kObject.y + (kObject.h - 1) / 2.0; }

// Analytic, aperiodic object texture over the ROI's local coordinates.
double ObjectTex(double u, double v) {
	double const s = 0.55 * std::sin(u * 0.34 + 1.7) * std::cos(v * 0.27 + 0.4) + 0.30 * std::sin((u + v) * 0.19 + 0.9) + 0.40 * std::sin(u * 0.92 + 2.2) * std::cos(v * 0.81 + 1.1) + 0.25 * std::sin(u * 1.7 - v * 1.3 + 0.5);
	return 128.0 + 46.0 * s;
}

double BackgroundTex(int x, int y) {
	return 96.0 + 10.0 * std::sin(x * 0.05) + 8.0 * std::cos(y * 0.06) + 5.0 * std::sin((x + y) * 0.021);
}

double OccluderTex(int x, int y) {
	return 55.0 + 6.0 * std::sin(x * 0.21 + 2.0) * std::cos(y * 0.17);
}

// The object translates at 1 px/frame through a stationary occluder strip.
struct OcclusionScene {
	int occluder_w = 18; // 45% of the 40 px object width
	static constexpr int kWidth = 128;
	static constexpr int kHeight = 64;
	static constexpr int kFrames = 50;
	static constexpr int kOccluderX = 70;

	// First frame whose object pixels overlap the occluder strip.
	static int OcclusionStartFrame() {
		return kOccluderX - (kObject.x + kObject.w - 1);
	}

	std::uint8_t Pixel(int x, int y, int frame) const {
		double const u = double(x) - ObjectCenterX(frame);
		double const v = double(y) - ObjectCenterY();
		double val;
		if (std::abs(u) <= (kObject.w - 1) / 2.0 && std::abs(v) <= (kObject.h - 1) / 2.0)
			val = ObjectTex(u, v);
		else
			val = BackgroundTex(x, y);
		if (x >= kOccluderX && x < kOccluderX + occluder_w)
			val = OccluderTex(x, y);
		return std::uint8_t(std::clamp(std::lround(val), 0L, 255L));
	}
};

void FillPatch(OcclusionScene const& scene, int frame, RoiRect roi,
			   GrayPatch& patch) {
	patch.frame = frame;
	patch.origin_x = roi.x;
	patch.origin_y = roi.y;
	patch.width = roi.w;
	patch.height = roi.h;
	patch.stride = roi.w;
	patch.gray.resize(size_t(roi.w) * roi.h);
	for (int y = 0; y < roi.h; ++y)
		for (int x = 0; x < roi.w; ++x)
			patch.gray[size_t(y) * roi.w + x] =
				scene.Pixel(roi.x + x, roi.y + y, frame);
}

TrackerSeed SeedFrom(GrayPatch const& patch) {
	TrackerSeed seed;
	seed.model = TrackModel::Translation;
	seed.roi = RoiRect{patch.origin_x, patch.origin_y, patch.width,
					   patch.height};
	seed.template_gray = patch.View();
	seed.seed_frame = patch.frame;
	return seed;
}

TrackStepResult StepAt(TranslationTrackerBackend& backend,
					   OcclusionScene const& scene, GrayPatch& patch,
					   int frame, double center_x, double center_y,
					   int radius) {
	auto const ex = int(std::floor(center_x - (kObject.w - 1) / 2.0 + 0.5));
	auto const ey = int(std::floor(center_y - (kObject.h - 1) / 2.0 + 0.5));
	RoiRect crop{ex - radius, ey - radius, kObject.w + 2 * radius,
				 kObject.h + 2 * radius};
	FillPatch(scene, frame, crop, patch);
	TrackStepRequest request;
	request.frame = frame;
	request.image = patch.View();
	request.image_origin_x = crop.x;
	request.image_origin_y = crop.y;
	request.search_center_x = center_x;
	request.search_center_y = center_y;
	return backend.Step(request); // patch alive through the call
}

} // namespace

TEST(motion_track_occlusion, inlier_rescore_tracks_through_partial_occlusion) {
	OcclusionScene scene;
	TranslationTrackerBackend backend;
	TranslationTrackerConfig config;
	// The bland occluder strip can satisfy the periodic-texture ambiguity
	// probe; that detector is not what this test exercises.
	config.ambiguity_check = false;
	backend.SetConfig(config);
	GrayPatch seed;
	FillPatch(scene, 0, kObject, seed);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));

	int const start = OcclusionScene::OcclusionStartFrame();
	ASSERT_GT(start, 0);
	ASSERT_LT(start, OcclusionScene::kFrames);

	double x = ObjectCenterX(0);
	double y = ObjectCenterY();
	GrayPatch patch;
	for (int frame = 1; frame < OcclusionScene::kFrames; ++frame) {
		auto step = StepAt(backend, scene, patch, frame, x, y, 10);
		ASSERT_EQ(TrackStatus::Ok, step.status) << "frame " << frame << " reason " << int(step.failure) << " conf " << step.confidence;
		x = step.candidate_center_x;
		y = step.candidate_center_y;
		double const err =
			std::hypot(x - ObjectCenterX(frame), y - ObjectCenterY());
		EXPECT_LT(err, 1.5) << "frame " << frame;
	}
}

TEST(motion_track_occlusion, inlier_rescore_disabled_fails_under_occlusion) {
	OcclusionScene scene;
	TranslationTrackerBackend backend;
	TranslationTrackerConfig config;
	config.robust_inlier_scoring = false;
	config.ambiguity_check = false;
	backend.SetConfig(config);
	GrayPatch seed;
	FillPatch(scene, 0, kObject, seed);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));

	int const start = OcclusionScene::OcclusionStartFrame();
	double x = ObjectCenterX(0);
	double y = ObjectCenterY();
	GrayPatch patch;
	bool saw_failure = false;
	for (int frame = 1; frame < OcclusionScene::kFrames; ++frame) {
		auto step = StepAt(backend, scene, patch, frame, x, y, 10);
		if (step.status != TrackStatus::Ok) {
			EXPECT_EQ(TrackFailureReason::NccLow, step.failure)
				<< "frame " << frame;
			EXPECT_GE(frame, start);
			saw_failure = true;
			break;
		}
		x = step.candidate_center_x;
		y = step.candidate_center_y;
	}
	// The run must die once the occluder covers enough of the object.
	EXPECT_TRUE(saw_failure);
}

TEST(motion_track_occlusion, inlier_rescore_rejects_near_total_occlusion) {
	// An occluder covering ~90% of the object exceeds the rejection cap and
	// must still fail instead of tracking the occluder's own texture.
	OcclusionScene scene;
	scene.occluder_w = 36;
	TranslationTrackerBackend backend;
	GrayPatch seed;
	FillPatch(scene, 0, kObject, seed);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));

	double x = ObjectCenterX(0);
	double y = ObjectCenterY();
	GrayPatch patch;
	bool saw_failure = false;
	for (int frame = 1; frame < OcclusionScene::kFrames; ++frame) {
		auto step = StepAt(backend, scene, patch, frame, x, y, 10);
		if (step.status != TrackStatus::Ok) {
			saw_failure = true;
			break;
		}
		x = step.candidate_center_x;
		y = step.candidate_center_y;
	}
	EXPECT_TRUE(saw_failure);
}

TEST(motion_track_occlusion, masked_subpixel_handles_window_at_image_edge) {
	// A tight crop pins the scan window to the single offset (0,0): the
	// masked subpixel neighbours then probe (-1, 0) and (0, -1), which lie
	// outside the image and must fall back to the peak's own score instead
	// of reading out of bounds.
	OcclusionScene scene;
	TranslationTrackerBackend backend;
	TranslationTrackerConfig config;
	config.ambiguity_check = false;
	backend.SetConfig(config);
	GrayPatch seed;
	FillPatch(scene, 0, kObject, seed);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));

	// Frame 30: the object spans x 50..89 with the 18 px occluder strip
	// covering 70..87 -- the same 45% occlusion the tracking test uses.
	constexpr int kFrame = 30;
	GrayPatch patch;
	auto step = StepAt(backend, scene, patch, kFrame, ObjectCenterX(kFrame),
					   ObjectCenterY(), 0);
	ASSERT_EQ(TrackStatus::Ok, step.status)
		<< "reason " << int(step.failure) << " conf " << step.confidence;
	double const err = std::hypot(step.candidate_center_x - ObjectCenterX(kFrame),
								  step.candidate_center_y - ObjectCenterY());
	EXPECT_LT(err, 1.5);
}

TEST(motion_track_occlusion, tie_break_prefers_offset_nearest_center) {
	// Two exact copies of one pattern inside a mildly textured field; both
	// windows score an identical 1.0, so only the tie rule can pick one.
	std::vector<std::uint8_t> templ(36);
	for (int v = 0; v < 6; ++v)
		for (int u = 0; u < 6; ++u)
			templ[size_t(v) * 6 + u] =
				std::uint8_t(60 + ((u * 7 + v * 13) % 23) * 6);

	constexpr int kW = 40, kH = 16;
	std::vector<std::uint8_t> image(size_t(kW) * kH);
	for (int y = 0; y < kH; ++y)
		for (int x = 0; x < kW; ++x)
			image[size_t(y) * kW + x] =
				std::uint8_t(90 + ((x * 5 + y * 3) % 17) * 2);
	for (int v = 0; v < 6; ++v)
		for (int u = 0; u < 6; ++u) {
			image[size_t(7 + v) * kW + (5 + u)] = templ[size_t(v) * 6 + u];
			image[size_t(7 + v) * kW + (28 + u)] = templ[size_t(v) * 6 + u];
		}

	GrayView const view{image.data(), kW, kW, kH};
	GrayView const template_view{templ.data(), 6, 6, 6};

	auto const first = FindBestNccScalar(template_view, view, 0, 0, 34, 10);
	ASSERT_TRUE(first.found);
	EXPECT_EQ(5, first.offset_x);
	EXPECT_EQ(7, first.offset_y);

	auto const nearer = FindBestNccScalar(template_view, view, 0, 0, 34, 10,
										  NccTieBreak{true, 28, 7});
	ASSERT_TRUE(nearer.found);
	EXPECT_EQ(28, nearer.offset_x);
	EXPECT_EQ(7, nearer.offset_y);
}

TEST(motion_track_occlusion, static_scene_never_accumulates_subpixel_creep) {
	SyntheticTranslationScene::Config config;
	config.width = 96;
	config.height = 64;
	config.frame_count = 40;
	config.object_at_frame0 = kObject;
	config.velocity_x = 0.0;
	config.velocity_y = 0.0;
	SyntheticTranslationScene scene(config);

	TranslationTrackerBackend backend;
	SyntheticFrameReader reader(scene);
	GrayPatch patch;
	ASSERT_EQ(FrameReadStatus::Ok, reader.FetchGray(0, kObject, patch).status);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(patch)));

	double const seed_x = kObject.x + (kObject.w - 1) / 2.0;
	double const seed_y = ObjectCenterY();
	for (int frame = 1; frame < 40; ++frame) {
		RoiRect crop{kObject.x - 8, kObject.y - 8, kObject.w + 16,
					 kObject.h + 16};
		ASSERT_EQ(FrameReadStatus::Ok,
				  reader.FetchGray(frame, crop, patch).status);
		TrackStepRequest request;
		request.frame = frame;
		request.image = patch.View();
		request.image_origin_x = crop.x;
		request.image_origin_y = crop.y;
		request.search_center_x = seed_x;
		request.search_center_y = seed_y;
		auto step = backend.Step(request);
		ASSERT_EQ(TrackStatus::Ok, step.status) << "frame " << frame << " reason " << int(step.failure) << " conf " << step.confidence;
		// Frames are pixel-identical, so every integer match is exact and the
		// subpixel stage must contribute exactly zero, every frame.
		EXPECT_DOUBLE_EQ(seed_x, step.candidate_center_x) << "frame " << frame;
		EXPECT_DOUBLE_EQ(seed_y, step.candidate_center_y) << "frame " << frame;
	}
}

TEST(motion_track_occlusion, select_inlier_blocks_rejects_sparse_outliers) {
	std::vector<double> residuals{1, 1, 1, 1, 1, 1, 1, 1, 50};
	std::vector<bool> kept;
	ASSERT_TRUE(SelectInlierBlocks(residuals, {}, kept));
	EXPECT_EQ(residuals.size(), kept.size());
	EXPECT_FALSE(kept[8]);
	for (size_t i = 0; i + 1 < kept.size(); ++i)
		EXPECT_TRUE(kept[i]);
}

TEST(motion_track_occlusion, select_inlier_blocks_keeps_uniformly_high_runs) {
	// Uniformly high residuals have MAD 0 at the median: nothing stands out
	// statistically, so nothing is rejected (the rescored NCC decides).
	std::vector<double> residuals{50, 50, 50, 50, 50, 50};
	std::vector<bool> kept;
	ASSERT_TRUE(SelectInlierBlocks(residuals, {}, kept));
	for (bool k : kept)
		EXPECT_TRUE(k);
}

TEST(motion_track_occlusion, select_inlier_blocks_needs_min_blocks) {
	std::vector<double> residuals{1, 2, 3};
	std::vector<bool> kept;
	EXPECT_FALSE(SelectInlierBlocks(residuals, {}, kept));
}

TEST(motion_track_occlusion, select_inlier_blocks_enforces_reject_cap) {
	// A bimodal 2/2 split cannot keep min_blocks inliers after dropping
	// either mode, so the selection fails rather than trim arbitrarily.
	std::vector<double> residuals{0, 0, 100, 100};
	std::vector<bool> kept;
	EXPECT_FALSE(SelectInlierBlocks(residuals, {}, kept));
}

TEST(motion_track_occlusion, select_inlier_pixels_gates_on_noise_scale) {
	// 55% of the pixels match exactly, 45% are occluder-level differences;
	// the low-side anchor sits on the matching side, so the gate keeps
	// exactly the matching pixels and the area floor is met.
	std::vector<double> diffs(20, 1.0);
	for (size_t i = 11; i < diffs.size(); ++i)
		diffs[i] = 60.0;
	std::vector<bool> kept;
	ASSERT_TRUE(SelectInlierPixels(diffs, {}, kept));
	EXPECT_EQ(diffs.size(), kept.size());
	for (size_t i = 0; i < kept.size(); ++i)
		EXPECT_EQ(i < 11, kept[i]) << "pixel " << i;
}

TEST(motion_track_occlusion, select_inlier_pixels_adapts_to_uniform_noise) {
	// Uniformly noisy inliers (a global brightness creep shifts every
	// difference alike): the median+MAD gate follows the noise scale, so
	// nothing is rejected even though the raw differences are far from 0.
	std::vector<double> diffs(16, 30.0);
	std::vector<bool> kept;
	ASSERT_TRUE(SelectInlierPixels(diffs, {}, kept));
	for (bool k : kept)
		EXPECT_TRUE(k);
}

TEST(motion_track_occlusion, select_inlier_pixels_enforces_area_cap) {
	// 75% occluded exceeds the 0.5 cap: the selection must fail rather
	// than rescore on a quarter of the template.
	std::vector<double> diffs(16, 1.0);
	for (size_t i = 4; i < diffs.size(); ++i)
		diffs[i] = 80.0;
	std::vector<bool> kept;
	EXPECT_FALSE(SelectInlierPixels(diffs, {}, kept));
}

TEST(motion_track_occlusion, select_inlier_pixels_needs_min_pixels) {
	std::vector<double> diffs{1.0, 2.0, 3.0};
	std::vector<bool> kept;
	EXPECT_FALSE(SelectInlierPixels(diffs, {}, kept));
}

// --- Held/duplicate-frame pre-check --------------------------------------
// The scenes and assertions are ours; the held-frame concept is adapted
// from croni1012/Aegisub src/typesetting_auto_motion.cpp (ISC license).

namespace {

// Deterministic per-frame luma dither: every pixel is nudged by +-1..3,
// redrawn each frame (encoder noise, not a fixed pattern). Keyed on the
// frame and the absolute pixel position, so independent crops of one frame
// agree.
std::uint8_t ApplyLumaDither(int frame, int x, int y, std::uint8_t base) {
	std::uint64_t z = std::uint64_t(frame + 1) * 0x9E3779B97F4A7C15ull ^ std::uint64_t(x) * 0xBF58476D1CE4E5B9ull ^ (std::uint64_t(y) * 0x94D049BB133111EBull + 1);
	z ^= z >> 33;
	z *= 0x517CC1B727220A95ull;
	z ^= z >> 29;
	int const magnitude = int(z % 3ull) + 1;
	int const sign = ((z >> 4) & 1) ? 1 : -1;
	return std::uint8_t(std::clamp(int(base) + sign * magnitude, 0, 255));
}

// Noisy-static scene shaped like static_scene_never_accumulates_subpixel_
// creep: pixel-identical frames, then a fresh +-1..3 dither applied to
// every fetched crop. The search center stays pinned at the seed center.
std::vector<TrackStepResult> RunNoisyStaticSteps(
	TranslationTrackerBackend& backend) {
	SyntheticTranslationScene::Config config;
	config.width = 96;
	config.height = 64;
	config.frame_count = 40;
	config.object_at_frame0 = kObject;
	config.velocity_x = 0.0;
	config.velocity_y = 0.0;
	SyntheticTranslationScene scene(config);
	SyntheticFrameReader reader(scene);

	GrayPatch patch;
	if (reader.FetchGray(0, kObject, patch).status != FrameReadStatus::Ok)
		return {};
	if (backend.Reset(SeedFrom(patch)) != TrackStatus::Ok)
		return {};

	double const seed_x = kObject.x + (kObject.w - 1) / 2.0;
	double const seed_y = ObjectCenterY();
	std::vector<TrackStepResult> steps;
	for (int frame = 1; frame < 40; ++frame) {
		RoiRect crop{kObject.x - 8, kObject.y - 8, kObject.w + 16,
					 kObject.h + 16};
		if (reader.FetchGray(frame, crop, patch).status != FrameReadStatus::Ok)
			return {};
		for (int y = 0; y < crop.h; ++y)
			for (int x = 0; x < crop.w; ++x)
				patch.gray[size_t(y) * crop.w + x] =
					ApplyLumaDither(frame, crop.x + x, crop.y + y,
									patch.gray[size_t(y) * crop.w + x]);
		TrackStepRequest request;
		request.frame = frame;
		request.image = patch.View();
		request.image_origin_x = crop.x;
		request.image_origin_y = crop.y;
		request.search_center_x = seed_x;
		request.search_center_y = seed_y;
		steps.push_back(backend.Step(request));
		if (steps.back().status != TrackStatus::Ok)
			break;
	}
	return steps;
}

} // namespace

TEST(motion_track_occlusion, held_frame_check_noisy_static_scene_keeps_exact_center) {
	// Encoder luma dither (+-1..3, redrawn every frame) over pixel-identical
	// static content: the held-frame pre-check must keep the previous
	// integer offset with exactly zero subpixel contribution, so the dither
	// can neither jitter a single frame nor accumulate a creep.
	TranslationTrackerBackend backend; // default config: held_frame_check on
	auto const steps = RunNoisyStaticSteps(backend);
	ASSERT_EQ(size_t{39}, steps.size());

	double const seed_x = kObject.x + (kObject.w - 1) / 2.0;
	double const seed_y = ObjectCenterY();
	for (size_t i = 0; i < steps.size(); ++i) {
		auto const& step = steps[i];
		int const frame = int(i) + 1;
		ASSERT_EQ(TrackStatus::Ok, step.status)
			<< "frame " << frame << " reason " << int(step.failure) << " conf "
			<< step.confidence;
		if (frame == 1) {
			// The first Step after Reset has no previous match: the full
			// search runs, its integer peak already sits at the seed spot,
			// and only the subpixel fit can wobble within the noise.
			EXPECT_NEAR(seed_x, step.candidate_center_x, 0.25)
				<< "frame " << frame;
			EXPECT_NEAR(seed_y, step.candidate_center_y, 0.25)
				<< "frame " << frame;
			continue;
		}
		// Held frames reuse the previous integer offset exactly.
		EXPECT_DOUBLE_EQ(seed_x, step.candidate_center_x) << "frame " << frame;
		EXPECT_DOUBLE_EQ(seed_y, step.candidate_center_y) << "frame " << frame;
	}
}

TEST(motion_track_occlusion, held_frame_check_does_not_swallow_real_motion) {
	// 1 px/frame object motion changes the previous match window far beyond
	// the dither gate, so the pre-check must never hold: every frame runs
	// the full search and the centers still follow the analytic motion.
	SyntheticTranslationScene::Config config;
	config.width = 96;
	config.height = 64;
	config.frame_count = 20;
	config.object_at_frame0 = kObject;
	config.velocity_x = 1.0;
	SyntheticTranslationScene scene(config);

	TranslationTrackerBackend backend; // default config: held_frame_check on
	SyntheticFrameReader reader(scene);
	GrayPatch patch;
	ASSERT_EQ(FrameReadStatus::Ok, reader.FetchGray(0, kObject, patch).status);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(patch)));

	double x = kObject.x + (kObject.w - 1) / 2.0;
	double y = ObjectCenterY();
	for (int frame = 1; frame < 20; ++frame) {
		auto const ex = int(std::floor(x - (kObject.w - 1) / 2.0 + 0.5));
		auto const ey = int(std::floor(y - (kObject.h - 1) / 2.0 + 0.5));
		RoiRect crop{ex - 8, ey - 8, kObject.w + 16, kObject.h + 16};
		ASSERT_EQ(FrameReadStatus::Ok,
				  reader.FetchGray(frame, crop, patch).status);
		TrackStepRequest request;
		request.frame = frame;
		request.image = patch.View();
		request.image_origin_x = crop.x;
		request.image_origin_y = crop.y;
		request.search_center_x = x;
		request.search_center_y = y;
		auto step = backend.Step(request);
		ASSERT_EQ(TrackStatus::Ok, step.status)
			<< "frame " << frame << " reason " << int(step.failure) << " conf "
			<< step.confidence;
		EXPECT_NEAR(ObjectCenterX(frame), step.candidate_center_x, 0.25)
			<< "frame " << frame;
		EXPECT_NEAR(ObjectCenterY(), step.candidate_center_y, 0.25)
			<< "frame " << frame;
		x = step.candidate_center_x;
		y = step.candidate_center_y;
	}
}

TEST(motion_track_occlusion, held_frame_check_disabled_lets_noise_jitter) {
	// Config gate: with the check disabled the same noisy-static scene runs
	// the full search every frame, and the per-frame dither then moves the
	// subpixel fit off the exact center on at least one frame — exactly the
	// jitter the enabled run must not show. Only the toggle is asserted,
	// not any specific jitter value.
	TranslationTrackerBackend backend;
	TranslationTrackerConfig config;
	config.held_frame_check = false;
	backend.SetConfig(config);
	auto const steps = RunNoisyStaticSteps(backend);
	ASSERT_EQ(size_t{39}, steps.size());

	double const seed_x = kObject.x + (kObject.w - 1) / 2.0;
	double const seed_y = ObjectCenterY();
	bool saw_jitter = false;
	for (size_t i = 0; i < steps.size(); ++i) {
		int const frame = int(i) + 1;
		// The gates stay healthy under +-1..3 dither; only the estimate
		// jitters.
		ASSERT_EQ(TrackStatus::Ok, steps[i].status)
			<< "frame " << frame << " reason " << int(steps[i].failure);
		if (frame >= 2 && (steps[i].candidate_center_x != seed_x || steps[i].candidate_center_y != seed_y))
			saw_jitter = true;
	}
	EXPECT_TRUE(saw_jitter);
}
