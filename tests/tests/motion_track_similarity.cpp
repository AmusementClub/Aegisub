#include <main.h>

#include "../../src/motion_track/session.h"
#include "../../src/motion_track/similarity_backend.h"
#include "../../src/motion_track/types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
using namespace aegisub::motion_track;

RoiRect const kObj{60, 40, 44, 32};
constexpr double kPi = 3.14159265358979323846;

std::uint8_t Clamp8(double v) {
	return static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0));
}

// Anisotropic object texture: gradients in several directions make both
// rotation and scale observable.
double ObjectTex(double u, double v) {
	double const s = 0.5 * std::sin(u * 0.31 + 1.7) * std::cos(v * 0.23 + 0.4)
	              + 0.3 * std::sin((u + v) * 0.17 + 0.9)
	              + 0.45 * std::sin(u * 0.83 + 2.2) * std::cos(v * 0.71 + 1.1);
	return 128.0 + 46.0 * s;
}

double BackgroundTex(int x, int y) {
	return 96.0 + 10.0 * std::sin(x * 0.05) + 8.0 * std::cos(y * 0.06);
}

struct SimilarityMotion {
	double cx = 0.0, cy = 0.0;
	double rot = 0.0; // radians
	double scale = 1.0;
};

// Deterministic scene whose object translates, rotates and scales smoothly.
class SimilarityScene {
public:
	SimilarityScene(double vx, double vy, double w_rot, double w_scale,
	                double texture_margin = 0.0, double texture_frequency = 1.0)
	: vx_(vx), vy_(vy), w_rot_(w_rot), w_scale_(w_scale), texture_margin_(texture_margin), texture_frequency_(texture_frequency) {}

	int Width() const { return 160; }
	int Height() const { return 120; }
	int Frames() const { return 30; }

	SimilarityMotion MotionAt(int frame) const {
		SimilarityMotion m;
		m.cx = kObj.x + (kObj.w - 1) / 2.0 + vx_ * frame;
		m.cy = kObj.y + (kObj.h - 1) / 2.0 + vy_ * frame;
		m.rot = w_rot_ * frame;
		m.scale = 1.0 + w_scale_ * frame;
		return m;
	}

	std::uint8_t Pixel(int x, int y, int frame) const {
		auto const m = MotionAt(frame);
		double const dx = x - m.cx;
		double const dy = y - m.cy;
		double const c = std::cos(-m.rot), s = std::sin(-m.rot);
		double const u = (c * dx - s * dy) / m.scale;
		double const v = (s * dx + c * dy) / m.scale;
		double const hw = (kObj.w - 1) / 2.0;
		double const hh = (kObj.h - 1) / 2.0;
		if (std::abs(u) <= hw + texture_margin_ && std::abs(v) <= hh + texture_margin_)
			return Clamp8(ObjectTex(u * texture_frequency_, v * texture_frequency_));
		return Clamp8(BackgroundTex(x, y));
	}

private:
	double vx_, vy_, w_rot_, w_scale_, texture_margin_, texture_frequency_;
};

class SimilarityFrameReader final : public MotionFrameReader {
public:
	explicit SimilarityFrameReader(SimilarityScene const& scene)
	: scene_(scene) {}

	FrameReadResult FetchGray(int frame, RoiRect roi, GrayPatch& out) override {
		if (frame < 0 || frame >= scene_.Frames())
			return FrameReadResult{FrameReadStatus::FrameUnavailable,
			                       "frame out of range"};
		int const w = std::max(roi.w, 0);
		int const h = std::max(roi.h, 0);
		out.frame = frame;
		out.origin_x = roi.x;
		out.origin_y = roi.y;
		out.width = w;
		out.height = h;
		out.stride = w;
		out.gray.assign(size_t(w) * size_t(h), 128);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x) {
				int const sx = roi.x + x;
				int const sy = roi.y + y;
				if (sx >= 0 && sy >= 0 && sx < scene_.Width()
				    && sy < scene_.Height())
					out.gray[size_t(y) * w + size_t(x)] =
					    scene_.Pixel(sx, sy, frame);
			}
		return FrameReadResult{FrameReadStatus::Ok, {}};
	}

private:
	SimilarityScene const& scene_;
};

TrackerSeed MakeSeed(GrayPatch const& patch) {
	TrackerSeed seed;
	seed.model = TrackModel::Similarity;
	seed.roi = RoiRect{patch.origin_x, patch.origin_y, patch.width,
	                   patch.height};
	seed.template_gray = patch.View();
	seed.seed_frame = patch.frame;
	return seed;
}

GrayPatch SeedPatch(SimilarityScene const& scene) {
	SimilarityFrameReader reader(scene);
	GrayPatch patch;
	EXPECT_EQ(FrameReadStatus::Ok, reader.FetchGray(0, kObj, patch).status);
	return patch;
}

TrackStepResult StepSim(SimilarityTrackerBackend& backend,
                        SimilarityScene const& scene, int frame,
                        SimilarityMotion const& expected, double init_rot,
                        double init_scale, int radius) {
	auto const ex = int(std::floor(
	    expected.cx - (kObj.w - 1) / 2.0 + 0.5));
	auto const ey = int(std::floor(
	    expected.cy - (kObj.h - 1) / 2.0 + 0.5));
	RoiRect crop{ex - radius, ey - radius, kObj.w + 2 * radius,
	             kObj.h + 2 * radius};
	SimilarityFrameReader reader(scene);
	GrayPatch patch;
	if (reader.FetchGray(frame, crop, patch).status != FrameReadStatus::Ok)
		return TrackStepResult{};

	TrackStepRequest request;
	request.frame = frame;
	request.image = patch.View();
	request.image_origin_x = crop.x;
	request.image_origin_y = crop.y;
	request.search_center_x = expected.cx;
	request.search_center_y = expected.cy;
	request.init_rotation = init_rot;
	request.init_scale = init_scale;
	return backend.Step(request);
}

double EstimatedRotation(TrackStepResult const& r) {
	return std::atan2(r.transform.matrix[3], r.transform.matrix[0]);
}

double EstimatedScale(TrackStepResult const& r) {
	return std::hypot(r.transform.matrix[0], r.transform.matrix[3]);
}

// Runs the backend over frames [1..last] chaining the previous estimate as
// the init pose (exactly what the session does); caller must have Reset the
// backend with the scene's frame-0 seed.
std::vector<TrackStepResult> TrackOverScene(SimilarityTrackerBackend& backend,
                                            SimilarityScene const& scene,
                                            int last, int radius = 16) {
	std::vector<TrackStepResult> results;
	double rot = 0.0;
	double scale = 1.0;
	for (int frame = 1; frame <= last; ++frame) {
		auto const expected = scene.MotionAt(frame);
		auto result = StepSim(backend, scene, frame, expected, rot, scale,
		                      radius);
		EXPECT_EQ(TrackStatus::Ok, result.status) << "frame " << frame << " failure=" << int(result.failure) << " conf=" << result.confidence << " res=" << result.residual;
		if (result.status != TrackStatus::Ok) break;
		rot = EstimatedRotation(result);
		scale = EstimatedScale(result);
		results.push_back(result);
	}
	return results;
}
}

TEST(motion_track_similarity, model_name_and_reset_validation) {
	SimilarityTrackerBackend backend;
	EXPECT_EQ(TrackModel::Similarity, backend.Model());
	EXPECT_EQ("ecc-similarity", backend.Name());
	EXPECT_EQ(0u, backend.TemplateHash());

	TrackerSeed bad_model;
	bad_model.model = TrackModel::Translation;
	EXPECT_EQ(TrackStatus::Unsupported, backend.Reset(bad_model));

	TrackerSeed bad_view;
	bad_view.model = TrackModel::Similarity;
	EXPECT_EQ(TrackStatus::InvalidInput, backend.Reset(bad_view));

	SimilarityScene scene(1.0, 0.0, 0.0, 0.0);
	EXPECT_EQ(TrackStatus::Ok,
	          backend.Reset(MakeSeed(SeedPatch(scene))));
	EXPECT_NE(0u, backend.TemplateHash());

	auto const motion = scene.MotionAt(1);
	auto const result = StepSim(backend, scene, 1, motion, 0.0, 1.0, 16);
	EXPECT_EQ(TrackStatus::Ok, result.status);
	EXPECT_GT(result.confidence, 0.9);
	EXPECT_NEAR(0.0, EstimatedRotation(result), 1e-3);
	EXPECT_NEAR(1.0, EstimatedScale(result), 0.01);
	EXPECT_NEAR(motion.cx, result.candidate_center_x, 0.3);
	EXPECT_NEAR(motion.cy, result.candidate_center_y, 0.3);
}

TEST(motion_track_similarity, recovers_cumulative_rotation) {
	// 0.5 deg/frame for 25 frames = 12.5 degrees total.
	SimilarityScene scene(1.5, 0.8, 0.5 * kPi / 180.0, 0.0);
	SimilarityTrackerBackend backend;
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(MakeSeed(SeedPatch(scene))));
	auto const results = TrackOverScene(backend, scene, 25);
	ASSERT_EQ(25u, results.size());
	for (int frame = 1; frame <= 25; ++frame) {
		auto const motion = scene.MotionAt(frame);
		auto const& r = results[size_t(frame - 1)];
		EXPECT_NEAR(motion.rot, EstimatedRotation(r), 0.02)
		    << "frame " << frame;
		EXPECT_NEAR(1.0, EstimatedScale(r), 0.02) << "frame " << frame;
		EXPECT_NEAR(motion.cx, r.candidate_center_x, 0.6)
		    << "frame " << frame;
		EXPECT_NEAR(motion.cy, r.candidate_center_y, 0.6)
		    << "frame " << frame;
	}
}

TEST(motion_track_similarity, recovers_cumulative_scale) {
	// +0.5%/frame for 25 frames = 1.125 total.
	SimilarityScene scene(1.0, -0.6, 0.0, 0.005);
	SimilarityTrackerBackend backend;
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(MakeSeed(SeedPatch(scene))));
	auto const results = TrackOverScene(backend, scene, 25);
	ASSERT_EQ(25u, results.size());
	for (int frame = 1; frame <= 25; ++frame) {
		auto const motion = scene.MotionAt(frame);
		auto const& r = results[size_t(frame - 1)];
		EXPECT_NEAR(motion.scale, EstimatedScale(r), 0.02)
		    << "frame " << frame;
		EXPECT_NEAR(0.0, EstimatedRotation(r), 0.02) << "frame " << frame;
		EXPECT_NEAR(motion.cx, r.candidate_center_x, 0.6)
		    << "frame " << frame;
	}
}

TEST(motion_track_similarity, recovers_combined_rotation_scale_translation) {
	SimilarityScene scene(1.2, 0.9, 0.3 * kPi / 180.0, 0.004);
	SimilarityTrackerBackend backend;
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(MakeSeed(SeedPatch(scene))));
	auto const results = TrackOverScene(backend, scene, 25);
	ASSERT_EQ(25u, results.size());
	for (int frame = 1; frame <= 25; ++frame) {
		auto const motion = scene.MotionAt(frame);
		auto const& r = results[size_t(frame - 1)];
		EXPECT_NEAR(motion.rot, EstimatedRotation(r), 0.02)
		    << "frame " << frame;
		EXPECT_NEAR(motion.scale, EstimatedScale(r), 0.02)
		    << "frame " << frame;
		EXPECT_NEAR(motion.cx, r.candidate_center_x, 0.6)
		    << "frame " << frame;
		EXPECT_NEAR(motion.cy, r.candidate_center_y, 0.6)
		    << "frame " << frame;
	}
}

TEST(motion_track_similarity, converges_from_identity_on_moderate_pose) {
	// One step from the identity init with a substantial pose difference:
	// the Gauss-Newton basin must cover it without a prior. ~8 degrees,
	// scale 1.08, small translation by frame 27.
	SimilarityScene big(1.0, 0.5, 8.0 * kPi / 180.0 / 27.0, 0.08 / 27.0);
	SimilarityTrackerBackend backend;
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(MakeSeed(SeedPatch(big))));
	auto const motion = big.MotionAt(27);
	auto const result = StepSim(backend, big, 27, motion, 0.0, 1.0, 20);
	EXPECT_EQ(TrackStatus::Ok, result.status);
	EXPECT_NEAR(motion.rot, EstimatedRotation(result), 0.03);
	EXPECT_NEAR(motion.scale, EstimatedScale(result), 0.03);
	EXPECT_NEAR(motion.cx, result.candidate_center_x, 0.8);
	EXPECT_NEAR(motion.cy, result.candidate_center_y, 0.8);
}

TEST(motion_track_similarity, subpixel_pose_stays_stable_through_scale_and_direction_changes) {
	// Keep the ROI inside the texture. Measure rendered-point motion against
	// analytic truth, including immediate reversals, with the previous estimate
	// (not the true center) initializing the next frame.
	for (double scale_step : {-0.008, 0.008}) {
		SCOPED_TRACE(scale_step);
		SimilarityScene scene(0.23, -0.17, 0.003, scale_step, 4.0, 2.0);
		SimilarityTrackerBackend backend;
		ASSERT_EQ(TrackStatus::Ok, backend.Reset(MakeSeed(SeedPatch(scene))));
		auto prior = scene.MotionAt(0);
		double previous_error_x[5] = {}, previous_error_y[5] = {};
		double max_error = 0.0, max_step_error = 0.0;
		for (int step = 1; step <= 50; ++step) {
			int const frame = step <= 25 ? step : 50 - step;
			auto const truth = scene.MotionAt(frame);
			auto const result = StepSim(backend, scene, frame, prior,
										prior.rot, prior.scale, 16);
			ASSERT_EQ(TrackStatus::Ok, result.status) << "step " << step;
			for (int point = 0; point < 5; ++point) {
				double const x = point == 0 ? 0.0 : (point & 1 ? -1.0 : 1.0) * (kObj.w - 1) / 2.0;
				double const y = point == 0 ? 0.0 : (point & 2 ? -1.0 : 1.0) * (kObj.h - 1) / 2.0;
				auto const& m = result.transform.matrix;
				double const error_x = result.candidate_center_x + m[0] * x + m[1] * y -
									   (truth.cx + truth.scale * (std::cos(truth.rot) * x - std::sin(truth.rot) * y));
				double const error_y = result.candidate_center_y + m[3] * x + m[4] * y -
									   (truth.cy + truth.scale * (std::sin(truth.rot) * x + std::cos(truth.rot) * y));
				max_error = std::max(max_error, std::hypot(error_x, error_y));
				max_step_error = std::max(max_step_error,
										  std::hypot(error_x - previous_error_x[point], error_y - previous_error_y[point]));
				previous_error_x[point] = error_x;
				previous_error_y[point] = error_y;
			}
			prior = {.cx = result.candidate_center_x, .cy = result.candidate_center_y, .rot = EstimatedRotation(result), .scale = EstimatedScale(result)};
		}
		EXPECT_LT(max_error, 0.15);
		EXPECT_LT(max_step_error, 0.12);
		RecordProperty(scale_step < 0 ? "shrink_max_error" : "grow_max_error", std::to_string(max_error));
		RecordProperty(scale_step < 0 ? "shrink_max_step_error" : "grow_max_step_error", std::to_string(max_step_error));
	}
}

TEST(motion_track_similarity, session_keeps_enlarged_rotated_template_inside_search_crop) {
	SimilarityScene scene(0.0, 0.0, 0.004, 0.04, 4.0);
	SimilarityFrameReader reader(scene);
	SimilarityTrackerBackend backend;
	SessionDomains domains;
	domains.decode_interval = {.first = 0, .last = 29};
	domains.direction_domain = {.first = 0, .last = 29};
	domains.video_frame_count = scene.Frames();
	domains.storage_width = scene.Width();
	domains.storage_height = scene.Height();
	domains.search_radius_override = 4;
	auto lease = VideoProviderLeaseState::Create();
	auto handle = lease->Acquire();
	MotionTrackSession session(domains, kObj, TrackDirection::Forward, TrackModel::Similarity);
	auto const seed = scene.MotionAt(0);
	session.SetBackendSeed(0, kObj, seed.cx, seed.cy);
	ASSERT_EQ(AnalyzeStopReason::Completed, session.RunAnalyze(reader, backend, handle, {}));
	auto const snapshot = session.Capture();
	for (int frame = 1; frame <= 29; ++frame) {
		auto const *sample = FindSample(snapshot->samples, frame);
		ASSERT_NE(nullptr, sample) << "frame " << frame;
		ASSERT_EQ(TrackStatus::Ok, sample->status) << "frame " << frame;
		auto const truth = scene.MotionAt(frame);
		EXPECT_NEAR(truth.scale, std::hypot(sample->transform.matrix[0], sample->transform.matrix[3]), 0.01);
		EXPECT_NEAR(truth.rot, std::atan2(sample->transform.matrix[3], sample->transform.matrix[0]), 0.01);
		EXPECT_NEAR(truth.cx, sample->center_x, 0.15);
		EXPECT_NEAR(truth.cy, sample->center_y, 0.15);
	}
}

TEST(motion_track_similarity, partial_occlusion_keeps_pose_but_full_occlusion_is_rejected) {
	for (double motion : {0.0, 1.0}) {
		SCOPED_TRACE(motion);
		SimilarityScene scene(0.3 * motion, -0.2 * motion, 0.003 * motion, 0.005 * motion, 4.0);
		SimilarityTrackerBackend backend;
		ASSERT_EQ(TrackStatus::Ok, backend.Reset(MakeSeed(SeedPatch(scene))));
		SimilarityFrameReader reader(scene);
		GrayPatch image;
		RoiRect const crop{.x = kObj.x - 16, .y = kObj.y - 16, .w = kObj.w + 32, .h = kObj.h + 32};
		ASSERT_EQ(FrameReadStatus::Ok, reader.FetchGray(1, crop, image).status);
		for (int y = 16; y < 16 + kObj.h; ++y)
			for (int x = 16 + 3 * kObj.w / 4; x < 16 + kObj.w; ++x)
				image.gray[static_cast<size_t>(y) * image.stride + x] = 240;
		TrackStepRequest request;
		request.frame = 1;
		request.image = image.View();
		request.image_origin_x = crop.x;
		request.image_origin_y = crop.y;
		request.search_center_x = scene.MotionAt(0).cx;
		request.search_center_y = scene.MotionAt(0).cy;
		auto const partial = backend.Step(request);
		ASSERT_EQ(TrackStatus::Ok, partial.status);
		auto const truth = scene.MotionAt(1);
		EXPECT_NEAR(truth.cx, partial.candidate_center_x, 0.15);
		EXPECT_NEAR(truth.cy, partial.candidate_center_y, 0.15);
		EXPECT_NEAR(truth.rot, EstimatedRotation(partial), 0.005);
		EXPECT_NEAR(truth.scale, EstimatedScale(partial), 0.005);
		std::ranges::fill(image.gray, 128);
		auto const full = backend.Step(request);
		EXPECT_EQ(TrackStatus::Failed, full.status);
		EXPECT_EQ(TrackFailureReason::NccLow, full.failure);
	}
}

TEST(motion_track_similarity, sparse_edges_preserve_motion_or_fail_when_unobservable) {
	// Most pixels still match before alignment. The few changed edges carry
	// the motion; a zero median residual must not classify them as occlusion.
	constexpr int radius = 16;
	int const width = kObj.w + 2 * radius;
	int const height = kObj.h + 2 * radius;
	std::vector<std::uint8_t> pixels(static_cast<size_t>(kObj.w) * kObj.h, 30);
	for (int y = 10; y < 22; ++y)
		for (int x = 12; x < 28; ++x)
			pixels[static_cast<size_t>(y) * kObj.w + x] = 200;
	TrackerSeed seed;
	seed.model = TrackModel::Similarity;
	seed.roi = kObj;
	seed.template_gray = {.data = pixels.data(), .stride = kObj.w, .width = kObj.w, .height = kObj.h};
	seed.seed_frame = 0;
	for (bool horizontal : {false, true}) {
		SCOPED_TRACE(horizontal);
		for (int shift : {-5, -3, -1, 0, 1, 3, 5}) {
			SCOPED_TRACE(shift);
			int const dx = horizontal ? shift : 0;
			int const dy = horizontal ? 0 : shift;
			std::vector<std::uint8_t> image(static_cast<size_t>(width) * height, 30);
			for (int y = 10; y < 22; ++y)
				for (int x = 12; x < 28; ++x)
					image[static_cast<size_t>(y + radius + dy) * width + x + radius + dx] = 200;
			SimilarityTrackerBackend backend;
			ASSERT_EQ(TrackStatus::Ok, backend.Reset(seed));
			TrackStepRequest request;
			request.frame = 1;
			request.image = {.data = image.data(), .stride = width, .width = width, .height = height};
			request.image_origin_x = kObj.x - radius;
			request.image_origin_y = kObj.y - radius;
			request.search_center_x = kObj.x + (kObj.w - 1) / 2.0;
			request.search_center_y = kObj.y + (kObj.h - 1) / 2.0;
			auto const result = backend.Step(request);
			ASSERT_EQ(TrackStatus::Ok, result.status);
			EXPECT_NEAR(request.search_center_x + dx, result.candidate_center_x, 0.05);
			EXPECT_NEAR(request.search_center_y + dy, result.candidate_center_y, 0.05);
			EXPECT_NEAR(0.0, EstimatedRotation(result), 0.005);
			EXPECT_NEAR(1.0, EstimatedScale(result), 0.005);
			if (horizontal) {
				// A new foreground stripe leaves the object visible but can
				// remove all horizontal motion gradients from the robust fit.
				for (int y = 0; y < kObj.h; ++y)
					for (int x = 33; x < kObj.w; ++x)
						image[static_cast<size_t>(y + radius) * width + x + radius] = 240;
				auto const occluded = backend.Step(request);
				if (shift == 0) {
					ASSERT_EQ(TrackStatus::Ok, occluded.status);
					EXPECT_NEAR(request.search_center_x, occluded.candidate_center_x, 0.05);
					EXPECT_NEAR(request.search_center_y, occluded.candidate_center_y, 0.05);
					EXPECT_NEAR(0.0, EstimatedRotation(occluded), 0.005);
					EXPECT_NEAR(1.0, EstimatedScale(occluded), 0.005);
				}
				else {
					EXPECT_EQ(TrackStatus::Failed, occluded.status);
					EXPECT_EQ(TrackFailureReason::NccLow, occluded.failure);
				}
			}
		}
	}
}

TEST(motion_track_similarity, flat_template_fails_with_ncc_low) {
	SimilarityTrackerBackend backend;
	std::vector<std::uint8_t> flat(size_t(kObj.w) * kObj.h, 90);
	TrackerSeed seed;
	seed.model = TrackModel::Similarity;
	seed.roi = kObj;
	seed.template_gray = GrayView{flat.data(), kObj.w, kObj.w, kObj.h};
	seed.seed_frame = 0;
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(seed));

	SimilarityScene scene(1.0, 0.0, 0.0, 0.0);
	auto const motion = scene.MotionAt(1);
	auto const result = StepSim(backend, scene, 1, motion, 0.0, 1.0, 16);
	EXPECT_EQ(TrackStatus::Failed, result.status);
	EXPECT_EQ(TrackFailureReason::NccLow, result.failure);
}

TEST(motion_track_similarity, per_step_bound_violation_reports_jump) {
	// Tighten the per-step rotation bound below the true motion: the
	// refinement converges, but the sanity gate must reject the step.
	SimilarityScene turning(1.0, 0.0, 0.02, 0.0); // 0.02 rad at frame 1
	SimilarityTrackerConfig config;
	config.max_rotation_per_step = 0.01;
	SimilarityTrackerBackend strict(config);
	ASSERT_EQ(TrackStatus::Ok,
	          strict.Reset(MakeSeed(SeedPatch(turning))));
	auto const motion = turning.MotionAt(1);
	auto const result = StepSim(strict, turning, 1, motion, 0.0, 1.0, 16);
	EXPECT_EQ(TrackStatus::Failed, result.status);
	EXPECT_EQ(TrackFailureReason::JumpTooLarge, result.failure);
}

TEST(motion_track_similarity, session_runs_similarity_and_fills_transform) {
	SimilarityScene scene(1.2, 0.6, 0.4 * kPi / 180.0, 0.003);
	SimilarityFrameReader reader(scene);
	SimilarityTrackerBackend backend;

	SessionDomains domains;
	domains.decode_interval = FrameInterval{0, 29};
	domains.direction_domain = FrameInterval{0, 29};
	domains.video_frame_count = scene.Frames();
	domains.storage_width = scene.Width();
	domains.storage_height = scene.Height();
	domains.search_radius_override = 24;

	auto lease = VideoProviderLeaseState::Create();
	auto handle = lease->Acquire();

	MotionTrackSession session(domains, kObj, TrackDirection::Forward,
	                           TrackModel::Similarity);
	// Seed at frame 10: the ROI must sit on the object AS OF frame 10.
	RoiRect const seed_roi{int(60 + 1.2 * 10), int(40 + 0.6 * 10), 44, 32};
	double const seed_cx = seed_roi.x + (kObj.w - 1) / 2.0;
	double const seed_cy = seed_roi.y + (kObj.h - 1) / 2.0;
	session.SetBackendSeed(10, seed_roi, seed_cx, seed_cy);

	AnalyzeRunHooks hooks;
	auto const stop = session.RunAnalyze(reader, backend, handle, hooks);
	EXPECT_EQ(AnalyzeStopReason::Completed, stop);

	auto const snap = session.Capture();
	ASSERT_TRUE(snap != nullptr);
	EXPECT_EQ(TrackModel::Similarity, snap->model);
	EXPECT_EQ("ecc-similarity", snap->backend_name);

	for (int frame = 11; frame <= 29; ++frame) {
		auto const* sample = FindSample(snap->samples, frame);
		ASSERT_NE(nullptr, sample) << "frame " << frame;
		EXPECT_EQ(TrackStatus::Ok, sample->status) << "frame " << frame;
		auto const motion = scene.MotionAt(frame);
		double const rot = std::atan2(sample->transform.matrix[3],
		                              sample->transform.matrix[0]);
		double const scale = std::hypot(sample->transform.matrix[0],
		                                sample->transform.matrix[3]);
		EXPECT_NEAR(motion.rot - scene.MotionAt(10).rot, rot, 0.03)
		    << "frame " << frame;
		EXPECT_NEAR(motion.scale / scene.MotionAt(10).scale, scale, 0.03)
		    << "frame " << frame;
		EXPECT_NEAR(motion.cx, sample->center_x, 0.8) << "frame " << frame;
		EXPECT_NEAR(motion.cy, sample->center_y, 0.8) << "frame " << frame;
		EXPECT_NEAR(motion.cx - seed_cx, sample->transform.matrix[2], 1.0)
		    << "frame " << frame;
	}
}
