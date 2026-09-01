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
	SimilarityScene(double vx, double vy, double w_rot, double w_scale)
	: vx_(vx), vy_(vy), w_rot_(w_rot), w_scale_(w_scale) {}

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
		if (std::abs(u) <= hw && std::abs(v) <= hh)
			return Clamp8(ObjectTex(u, v));
		return Clamp8(BackgroundTex(x, y));
	}

private:
	double vx_, vy_, w_rot_, w_scale_;
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
