// Fade detection, phase A: tracking robustness during global fades, and
// phase B: the full visibility curve, session-level fade interval
// extraction (reusing align_video_fade's plateau/curve fitting) and \fad
// composition in the apply planner. The scenes and assertions are ours; the
// visibility semantics (least-squares contrast slope against the seed
// template) deliberately mirror the align_video_fade / FadeVisibilityScore
// naming so phase B can fit \fad timing on the recorded curve.

#include <main.h>

#include "../../src/align_video_fade.h"
#include "../../src/motion_track/apply_plan.h"
#include "../../src/motion_track/ncc.h"
#include "../../src/motion_track/session.h"
#include "../../src/motion_track/synthetic_frame_reader.h"
#include "../../src/motion_track/translation_backend.h"

#include <ass_dialogue.h>
#include <ass_file.h>
#include <ass_style.h>
#include <libaegisub/vfr.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace aegisub::motion_track;

RoiRect const kObject{24, 16, 40, 28};
constexpr int kFrames = 64;
constexpr double kFadeLevel = 128.0;  // global fade pulls every pixel here
constexpr double kResidualMax = 22.0; // TranslationTrackerConfig default

double SeedCenterX() { return kObject.x + (kObject.w - 1) / 2.0; }
double SeedCenterY() { return kObject.y + (kObject.h - 1) / 2.0; }

// Analytic, aperiodic object shape in [-1, 1]-ish. The default amplitude
// (FadeScene::amplitude = 90) is deliberately large: the median |template -
// faded window| of a global fade is (1 - f) * ~31 gray levels, so contrast
// below ~30% trips the residual gate and the fade -- invisible to zero-mean
// NCC -- becomes observable exactly where phase A needs it.
double FadeShape(double u, double v) {
	double const s = 0.55 * std::sin(u * 0.31 + 1.7) * std::cos(v * 0.26 + 0.4) + 0.30 * std::sin((u + v) * 0.18 + 0.9) + 0.40 * std::sin(u * 0.9 + 2.2) * std::cos(v * 0.8 + 1.1) + 0.25 * std::sin(u * 1.6 - v * 1.2 + 0.5);
	return s;
}

double FadeBackgroundTex(int x, int y) {
	return 96.0 + 10.0 * std::sin(x * 0.05) + 8.0 * std::cos(y * 0.06) + 5.0 * std::sin((x + y) * 0.021);
}

// Static object (optional integer per-frame x offset) under a global fade
// v' = level + f(frame) * (v - level). A global fade dims object and
// background alike, which is acceptable for phase A.
struct FadeScene {
	static constexpr int kWidth = 160;
	static constexpr int kHeight = 80;

	std::function<double(int)> factor; // 1 = no fade
	std::function<int(int)> offset_x;  // integer object shift, storage px
	// Texture amplitude around kFadeLevel; the seed template is generated
	// with the same value, so raising it raises the template's MAD (the
	// residual the fade must fight through) without changing the shape.
	double amplitude = 90.0;
	// Level the fade converges toward: 0 = fade to black, 255 = fade to
	// white, kFadeLevel = the mid-gray default. The object texture itself
	// is always generated around kFadeLevel (well-centered, no clipping).
	double fade_level = kFadeLevel;

	double Factor(int frame) const { return factor ? factor(frame) : 1.0; }
	int OffsetX(int frame) const { return offset_x ? offset_x(frame) : 0; }

	std::uint8_t RawPixel(int x, int y, int frame) const {
		double const u = double(x - (kObject.x + OffsetX(frame)));
		double const v = double(y - kObject.y);
		double const val = (u >= 0 && u < kObject.w && v >= 0 && v < kObject.h)
							   ? kFadeLevel + amplitude * FadeShape(u, v)
							   : FadeBackgroundTex(x, y);
		return std::uint8_t(std::clamp(std::lround(val), 0L, 255L));
	}

	std::uint8_t Pixel(int x, int y, int frame) const {
		double const f = Factor(frame);
		if (f >= 1.0)
			return RawPixel(x, y, frame);
		double const val =
			fade_level + f * (double(RawPixel(x, y, frame)) - fade_level);
		return std::uint8_t(std::clamp(std::lround(val), 0L, 255L));
	}

	// Analytic ground truth: the object footprint is the seed ROI shifted by
	// the per-frame offset, so the center moves with it.
	double GroundCenterX(int frame) const {
		return kObject.x + OffsetX(frame) + (kObject.w - 1) / 2.0;
	}
	double GroundCenterY() const { return SeedCenterY(); }
};

void FillPatch(FadeScene const& scene, int frame, RoiRect roi,
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
					   FadeScene const& scene, GrayPatch& patch, int frame,
					   double center_x, double center_y, int radius) {
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

double MedianAbsFromLevel(GrayPatch const& patch, double level) {
	std::vector<int> diffs;
	diffs.reserve(patch.gray.size());
	for (std::uint8_t p : patch.gray)
		diffs.push_back(std::abs(int(p) - int(std::lround(level))));
	auto const mid = diffs.begin() + (diffs.size() - 1) / 2;
	std::nth_element(diffs.begin(), mid, diffs.end());
	return double(*mid);
}

// First frame whose faded window trips the residual gate:
// (1 - f) * M > residual_max with M the seed's median |pixel - level| (the
// level the scene fades toward, not the texture's own center). The fade
// rounds each pixel by <= 0.5, so callers assert within +-1 frame.
int ExpectedResidualCrossing(FadeScene const& scene, GrayPatch const& seed) {
	double const m = MedianAbsFromLevel(seed, scene.fade_level);
	for (int frame = 1; frame < kFrames; ++frame)
		if ((1.0 - scene.Factor(frame)) * m > kResidualMax)
			return frame;
	return kFrames;
}

// Fade-held step signature under the phase B full curve: held steps publish
// confidence = the bounded slope -- exactly the fade_visibility value --
// while plain steps publish the NCC peak, which stays ~1.0 through a fade
// (zero-mean NCC is contrast-invariant) even while the measured slope
// drops. The < 0.999 guard excludes fully-visible plain frames where both
// legitimately equal 1.0.
bool IsFadeHeld(double confidence, double fade_visibility) {
	return fade_visibility >= 0.0 && fade_visibility < 0.999 && confidence == fade_visibility;
}

// MotionFrameReader view of a FadeScene for the session-level tests.
struct FadeSceneReader final : MotionFrameReader {
	FadeScene const& scene;
	explicit FadeSceneReader(FadeScene const& scene) : scene(scene) {}

	FrameReadResult FetchGray(int frame, RoiRect roi, GrayPatch& out) override {
		FillPatch(scene, frame, roi, out);
		return FrameReadResult{FrameReadStatus::Ok, {}};
	}
};

SessionDomains FadeDomains() {
	SessionDomains d;
	d.decode_interval = FrameInterval{0, kFrames - 1};
	d.direction_domain = FrameInterval{0, kFrames - 1};
	d.video_frame_count = kFrames;
	d.search_radius_override = 14;
	return d;
}

// Fade-out profile used by the hold and config-off tests: linear down to
// 8% contrast by frame 24, then constant to the end of the domain.
double FadeOutFactor(int frame) {
	return frame >= 24 ? 0.08 : 1.0 - 0.92 * frame / 24.0;
}

// Fade-out profile for the phase B interval tests: a fully-visible plateau
// up to (excluding) kPlateauEnd, a linear ramp down to the 8% floor across
// kRampFrames frames, then the constant floor. The plateau lets the seed
// anchor a confirmed fully-visible platform before the ramp starts,
// mirroring a user seeding on a visible object that fades later.
constexpr int kPlateauEnd = 20;
constexpr int kRampFrames = 24;
double LateFadeOutFactor(int frame) {
	if (frame < kPlateauEnd)
		return 1.0;
	if (frame < kPlateauEnd + kRampFrames)
		return 1.0 - 0.92 * (frame - kPlateauEnd) / kRampFrames;
	return 0.08;
}

// Last frame whose factor still sits inside the fully-visible plateau band
// (~0.5% below the platform level, the reference band on clean data): the
// expected full_visibility_frame of the fitted fade-out, derived from the
// ramp shape like ExpectedResidualCrossing derives the gate crossing.
int ExpectedLastFullyVisible(FadeScene const& scene) {
	for (int frame = 1; frame < kFrames; ++frame)
		if (scene.Factor(frame) < 0.995)
			return frame - 1;
	return kFrames - 1;
}

// First frame from the deep end whose factor rises above the fit's visible
// threshold (baseline + ~1% of the on/off scale -- the fitter's floored
// noise threshold on clean data): the expected outer_frame.
int ExpectedFirstVisible(FadeScene const& scene, double floor_level) {
	for (int frame = kFrames - 1; frame > 0; --frame)
		if (scene.Factor(frame) > floor_level + 0.01 * (1.0 - floor_level))
			return frame;
	return 0;
}

} // namespace

TEST(motion_track_fade, zero_mean_ncc_scalar_exports_contrast_slope) {
	// Unit contract of the slope export: identical -> 1.0, halved contrast
	// -> 0.5, and the pinned degenerate values (flat window = fully faded
	// 0.0, flat template = nothing to measure, safe 1.0).
	std::vector<std::uint8_t> templ(64), same(64), half(64), flat(64, 100);
	for (int v = 0; v < 8; ++v)
		for (int u = 0; u < 8; ++u) {
			// Even values: the halved window stays exactly representable.
			int const t = 90 + ((u * 5 + v * 11) % 17) * 4;
			templ[size_t(v) * 8 + u] = std::uint8_t(t);
			same[size_t(v) * 8 + u] = std::uint8_t(t);
			half[size_t(v) * 8 + u] =
				std::uint8_t(128 + (t - 128) / 2);
		}
	GrayView const tv{templ.data(), 8, 8, 8};
	GrayView const samev{same.data(), 8, 8, 8};
	GrayView const halfv{half.data(), 8, 8, 8};
	GrayView const flatv{flat.data(), 8, 8, 8};

	double ncc = 0.0;
	double slope = -1.0;
	ASSERT_TRUE(ZeroMeanNccScalar(tv, samev, 0, 0, ncc, &slope));
	EXPECT_NEAR(1.0, ncc, 1e-9);
	EXPECT_NEAR(1.0, slope, 1e-9);

	ASSERT_TRUE(ZeroMeanNccScalar(tv, halfv, 0, 0, ncc, &slope));
	// NCC is brightness/scale invariant: the halved window still scores 1.0
	// -- which is exactly why the slope is the fade signal.
	EXPECT_NEAR(1.0, ncc, 1e-9);
	EXPECT_NEAR(0.5, slope, 1e-9);

	slope = 123.0;
	EXPECT_FALSE(ZeroMeanNccScalar(tv, flatv, 0, 0, ncc, &slope));
	EXPECT_NEAR(0.0, slope, 1e-9);

	slope = 123.0;
	EXPECT_FALSE(ZeroMeanNccScalar(flatv, tv, 0, 0, ncc, &slope));
	EXPECT_NEAR(1.0, slope, 1e-9);
}

TEST(motion_track_fade, fade_out_holds_position_and_records_visibility) {
	// Fade to 8% contrast by frame 24, then static faded content to the end
	// of the domain: every step stays Ok, the hold reports the last accepted
	// position exactly (the design guarantees bit-identical centers during
	// the hold; accuracy within the standard subpixel tolerance), and the
	// recorded visibility follows the fade factor down.
	FadeScene scene;
	scene.factor = FadeOutFactor;

	TranslationTrackerBackend backend; // defaults: fade_detection on
	GrayPatch seed;
	FillPatch(scene, 0, kObject, seed);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));
	int const expected_enter = ExpectedResidualCrossing(scene, seed);

	double x = SeedCenterX();
	double y = SeedCenterY();
	GrayPatch patch;
	std::vector<TrackStepResult> steps;
	for (int frame = 1; frame < kFrames; ++frame) {
		auto step = StepAt(backend, scene, patch, frame, x, y, 12);
		ASSERT_EQ(TrackStatus::Ok, step.status)
			<< "frame " << frame << " reason " << int(step.failure) << " conf "
			<< step.confidence;
		x = step.candidate_center_x;
		y = step.candidate_center_y;
		steps.push_back(step);
	}
	ASSERT_EQ(size_t(kFrames - 1), steps.size());

	// The fade gate engages within +-1 frame of the computed residual
	// crossing. Phase B full curve: before that the fade is invisible to
	// the gates, so the steps are plain matches -- which now also carry a
	// measured visibility that tracks the analytic fade factor.
	size_t enter = steps.size();
	for (size_t i = 0; i < steps.size(); ++i)
		if (IsFadeHeld(steps[i].confidence, steps[i].fade_visibility)) {
			enter = i;
			break;
		}
	ASSERT_LT(enter, steps.size());
	int const enter_frame = int(enter) + 1;
	EXPECT_NEAR(double(expected_enter), enter_frame, 1.0);
	for (size_t i = 0; i < enter; ++i)
		EXPECT_NEAR(FadeOutFactor(int(i) + 1), steps[i].fade_visibility, 0.03)
			<< "frame " << i + 1;

	// During the hold the position is exactly constant (the stored accepted
	// center re-reported verbatim) and within the subpixel tolerance of the
	// seed center.
	for (size_t i = enter; i < steps.size(); ++i) {
		EXPECT_DOUBLE_EQ(steps[enter].candidate_center_x,
						 steps[i].candidate_center_x)
			<< "frame " << i + 1;
		EXPECT_DOUBLE_EQ(steps[enter].candidate_center_y,
						 steps[i].candidate_center_y)
			<< "frame " << i + 1;
	}
	EXPECT_NEAR(SeedCenterX(), steps[enter].candidate_center_x, 0.25);
	EXPECT_NEAR(SeedCenterY(), steps[enter].candidate_center_y, 0.25);

	// Visibility decreases monotonically (rounding-level wiggle allowed)
	// down to the fade floor, and the published confidence is the bounded
	// slope itself.
	for (size_t i = enter + 1; i < steps.size(); ++i)
		EXPECT_LE(steps[i].fade_visibility, steps[i - 1].fade_visibility + 0.005)
			<< "frame " << i + 1;
	EXPECT_NEAR(FadeOutFactor(kFrames - 1),
				steps.back().fade_visibility, 0.03);
	EXPECT_DOUBLE_EQ(steps.back().fade_visibility,
					 steps.back().confidence);
}

TEST(motion_track_fade, fade_recovery_resumes_tracking) {
	// Fade down to 10%, then back up above the exit slope; once the object
	// is fully visible again it starts moving 1 px/frame. The probe cadence
	// must resume normal tracking, and the final positions must match the
	// analytic ground truth.
	FadeScene scene;
	scene.factor = [](int frame) {
		if (frame <= 16)
			return std::max(0.1, 1.0 - 0.9 * frame / 16.0);
		if (frame >= 40)
			return 1.0;
		return 0.1 + 0.9 * (frame - 16) / 24.0;
	};
	scene.offset_x = [](int frame) { return frame >= 44 ? frame - 44 : 0; };

	TranslationTrackerBackend backend; // defaults: fade_detection on
	GrayPatch seed;
	FillPatch(scene, 0, kObject, seed);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));

	double x = SeedCenterX();
	double y = SeedCenterY();
	GrayPatch patch;
	bool saw_enter = false;
	bool saw_exit = false;
	for (int frame = 1; frame < kFrames; ++frame) {
		auto step = StepAt(backend, scene, patch, frame, x, y, 12);
		ASSERT_EQ(TrackStatus::Ok, step.status)
			<< "frame " << frame << " reason " << int(step.failure) << " conf "
			<< step.confidence;
		if (IsFadeHeld(step.confidence, step.fade_visibility))
			saw_enter = true;
		// Hysteresis: the hold only ends on a probe measuring slope >= the
		// exit threshold; that step carries the probe slope. Counted only
		// after the hold began, so fully-visible plain frames (visibility
		// legitimately 1.0) cannot satisfy it on their own.
		if (saw_enter && step.fade_visibility >= 0.85)
			saw_exit = true;
		// While the contrast is collapsed the position stays at the seed.
		if (frame >= 20 && frame <= 40 && step.fade_visibility >= 0.0) {
			EXPECT_NEAR(SeedCenterX(), step.candidate_center_x, 0.25)
				<< "frame " << frame;
			EXPECT_NEAR(SeedCenterY(), step.candidate_center_y, 0.25)
				<< "frame " << frame;
		}
		// After recovery the tracker follows the analytic motion again.
		if (frame >= 50) {
			EXPECT_NEAR(scene.GroundCenterX(frame), step.candidate_center_x,
						0.25)
				<< "frame " << frame;
			EXPECT_NEAR(scene.GroundCenterY(), step.candidate_center_y, 0.25)
				<< "frame " << frame;
		}
		x = step.candidate_center_x;
		y = step.candidate_center_y;
	}
	EXPECT_TRUE(saw_enter);
	EXPECT_TRUE(saw_exit);
}

TEST(motion_track_fade, unfaded_control_matches_plain_tracking) {
	// Control: the same scene with f = 1 throughout behaves exactly like
	// today positionally -- pixel-identical static frames, exact centers
	// from the first full search onward -- while phase B's full curve now
	// also measures visibility on those plain steps: ~1.0 everywhere, never
	// the -1 "not evaluated" sentinel.
	FadeScene scene;
	scene.factor = [](int) { return 1.0; };

	TranslationTrackerBackend backend; // defaults: fade_detection on
	GrayPatch seed;
	FillPatch(scene, 0, kObject, seed);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));

	double x = SeedCenterX();
	double y = SeedCenterY();
	GrayPatch patch;
	for (int frame = 1; frame < kFrames; ++frame) {
		auto step = StepAt(backend, scene, patch, frame, x, y, 12);
		ASSERT_EQ(TrackStatus::Ok, step.status)
			<< "frame " << frame << " reason " << int(step.failure);
		EXPECT_DOUBLE_EQ(SeedCenterX(), step.candidate_center_x)
			<< "frame " << frame;
		EXPECT_DOUBLE_EQ(SeedCenterY(), step.candidate_center_y)
			<< "frame " << frame;
		// The static frames are pixel-identical, so the measured slope is
		// exactly 1.0; a small epsilon absorbs integer-arithmetic wiggle.
		EXPECT_NEAR(1.0, step.fade_visibility, 1e-6) << "frame " << frame;
		x = step.candidate_center_x;
		y = step.candidate_center_y;
	}
}

TEST(motion_track_fade, fade_detection_disabled_fails_on_contrast_collapse) {
	// Config gate: with fade detection off the old failure mode returns --
	// the residual gate hard-fails once the contrast collapse pushes the
	// median |template - window| past residual_max.
	FadeScene scene;
	scene.factor = FadeOutFactor;

	TranslationTrackerBackend backend;
	TranslationTrackerConfig config;
	config.fade_detection = false;
	backend.SetConfig(config);
	GrayPatch seed;
	FillPatch(scene, 0, kObject, seed);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));
	int const expected_crossing = ExpectedResidualCrossing(scene, seed);

	double x = SeedCenterX();
	double y = SeedCenterY();
	GrayPatch patch;
	int first_failure = -1;
	int failures = 0;
	for (int frame = 1; frame < kFrames; ++frame) {
		auto step = StepAt(backend, scene, patch, frame, x, y, 12);
		if (step.status != TrackStatus::Ok) {
			if (first_failure < 0) {
				first_failure = frame;
				EXPECT_EQ(TrackFailureReason::ResidualHigh, step.failure)
					<< "frame " << frame;
			}
			++failures;
			continue; // keep stepping: the failure must persist
		}
		x = step.candidate_center_x;
		y = step.candidate_center_y;
	}
	ASSERT_GE(first_failure, 1);
	EXPECT_NEAR(double(expected_crossing), first_failure, 1.0);
	// The collapse only deepens, so every later frame fails too.
	EXPECT_EQ(kFrames - first_failure, failures);
}

TEST(motion_track_fade, displaced_target_while_faded_reports_lost) {
	// While faded the content is replaced by a displaced confident copy --
	// the target moved while invisible. The probe must report the target
	// lost (NccLow) within one probe interval instead of teleporting to the
	// new location.
	FadeScene scene;
	scene.factor = [](int frame) {
		return frame <= 12 ? std::max(0.15, 1.0 - 0.85 * frame / 12.0) : 0.15;
	};
	constexpr int kShiftFrame = 30;
	constexpr int kShiftPx = 6;
	scene.offset_x = [](int frame) {
		return frame >= kShiftFrame ? kShiftPx : 0;
	};

	TranslationTrackerBackend backend; // defaults: fade_detection on
	GrayPatch seed;
	FillPatch(scene, 0, kObject, seed);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));

	double x = SeedCenterX();
	double y = SeedCenterY();
	GrayPatch patch;
	int failure_frame = -1;
	for (int frame = 1; frame < kFrames; ++frame) {
		auto step = StepAt(backend, scene, patch, frame, x, y, 12);
		if (step.status != TrackStatus::Ok) {
			failure_frame = frame;
			EXPECT_EQ(TrackFailureReason::NccLow, step.failure)
				<< "frame " << frame;
			break;
		}
		// No teleport: until the failure the hold keeps the seed position.
		EXPECT_NEAR(SeedCenterX(), step.candidate_center_x, 0.25)
			<< "frame " << frame;
		EXPECT_NEAR(SeedCenterY(), step.candidate_center_y, 0.25)
			<< "frame " << frame;
		x = step.candidate_center_x;
		y = step.candidate_center_y;
	}
	// The failure surfaces after the shift and within one probe interval
	// (fade_probe_interval = 8) plus the shift frame itself.
	ASSERT_GE(failure_frame, kShiftFrame);
	EXPECT_LE(failure_frame, kShiftFrame + 8 + 1);
}

// --- Session integration ---------------------------------------------------

TEST(motion_track_fade, session_survives_fade_with_visibility_curve) {
	// Fade-held steps are normal Ok steps: the run completes, fades can no
	// longer kill the arm through consecutive failures, and the measured
	// visibility is copied into the published samples.
	FadeScene scene;
	scene.factor = FadeOutFactor;

	FadeSceneReader reader(scene);
	TranslationTrackerBackend backend; // defaults: fade_detection on
	MotionTrackSession session(FadeDomains(), kObject, TrackDirection::Forward,
							   TrackModel::Translation);
	session.SetBackendSeed(0, kObject, SeedCenterX(), SeedCenterY());

	auto lease = VideoProviderLeaseState::Create();
	auto handle = lease->Acquire();
	auto stop = session.RunAnalyze(reader, backend, handle, {});
	EXPECT_EQ(AnalyzeStopReason::Completed, stop);

	auto snap = session.Capture();
	ASSERT_EQ(size_t(kFrames), snap->samples.size());
	GrayPatch seed;
	FillPatch(scene, 0, kObject, seed);
	int const expected_enter = ExpectedResidualCrossing(scene, seed);

	int enter_frame = -1;
	for (auto const& s : snap->samples) {
		EXPECT_EQ(TrackStatus::Ok, s.status) << "frame " << s.frame;
		EXPECT_NEAR(SeedCenterX(), s.center_x, 0.25) << "frame " << s.frame;
		EXPECT_NEAR(SeedCenterY(), s.center_y, 0.25) << "frame " << s.frame;
		// Full curve: every published Ok sample carries a measurement.
		EXPECT_GE(s.fade_visibility, 0.0) << "frame " << s.frame;
		if (enter_frame < 0 && s.frame > 0 && IsFadeHeld(s.confidence, s.fade_visibility))
			enter_frame = s.frame;
	}
	// The seed sample is its own fully-visible reference.
	auto const *seed_sample = FindSample(snap->samples, 0);
	ASSERT_NE(nullptr, seed_sample);
	EXPECT_DOUBLE_EQ(1.0, seed_sample->fade_visibility);

	ASSERT_GE(enter_frame, 1);
	EXPECT_NEAR(double(expected_enter), enter_frame, 1.0);
	auto const *last = FindSample(snap->samples, kFrames - 1);
	ASSERT_NE(nullptr, last);
	EXPECT_NEAR(FadeOutFactor(kFrames - 1), last->fade_visibility, 0.03);
}

TEST(motion_track_fade, session_survives_fade_to_black_and_white) {
	// M1: a fade converging toward black (level 0) or white (255) drives
	// the median |template - window| at (1 - f) * median |t - L|, maximal
	// at the extrema -- for the default template (median ~128, MAD ~31) the
	// trip sits near slope 1 - 22/128 ~= 0.83, far above fade_enter_slope.
	// Before template_mad_ covered the extrema the ResidualHigh exemption
	// was armed only at ~1 - 22/31 ~= 0.29 -> max(0.6, 0.29) = 0.6, the
	// band (0.6, 0.83] hard-failed for more than kMaxConsecutiveFailures
	// frames and killed the arm mid-fade. With the fix the run completes,
	// every frame stays Ok and the held position is exactly constant.
	for (double level : {0.0, 255.0}) {
		SCOPED_TRACE("fade level " + std::to_string(int(level)));
		FadeScene scene;
		scene.fade_level = level;
		scene.factor = FadeOutFactor;

		FadeSceneReader reader(scene);
		TranslationTrackerBackend backend; // defaults: fade_detection on
		MotionTrackSession session(FadeDomains(), kObject,
								   TrackDirection::Forward,
								   TrackModel::Translation);
		session.SetBackendSeed(0, kObject, SeedCenterX(), SeedCenterY());

		auto lease = VideoProviderLeaseState::Create();
		auto handle = lease->Acquire();
		auto stop = session.RunAnalyze(reader, backend, handle, {});
		EXPECT_EQ(AnalyzeStopReason::Completed, stop);

		auto snap = session.Capture();
		ASSERT_EQ(size_t(kFrames), snap->samples.size());
		GrayPatch seed;
		FillPatch(scene, 0, kObject, seed);
		int const expected_enter = ExpectedResidualCrossing(scene, seed);
		EXPECT_GT(expected_enter, 1); // the extremal band is real
		EXPECT_LT(expected_enter, 12);

		int enter_frame = -1;
		for (auto const& s : snap->samples) {
			EXPECT_EQ(TrackStatus::Ok, s.status) << "frame " << s.frame;
			EXPECT_NEAR(SeedCenterX(), s.center_x, 0.25) << "frame " << s.frame;
			EXPECT_NEAR(SeedCenterY(), s.center_y, 0.25) << "frame " << s.frame;
			if (enter_frame < 0 && s.frame > 0 && IsFadeHeld(s.confidence, s.fade_visibility))
				enter_frame = s.frame;
		}
		// The fade gate engages within +-1 frame of the extremal residual
		// crossing, and the hold is exactly static from there on.
		ASSERT_GE(enter_frame, 1);
		EXPECT_NEAR(double(expected_enter), enter_frame, 1.0);
		auto const *enter = FindSample(snap->samples, enter_frame);
		ASSERT_NE(nullptr, enter);
		for (auto const& s : snap->samples) {
			if (s.frame < enter_frame)
				continue;
			EXPECT_DOUBLE_EQ(enter->center_x, s.center_x) << "frame " << s.frame;
			EXPECT_DOUBLE_EQ(enter->center_y, s.center_y) << "frame " << s.frame;
		}
		auto const *last = FindSample(snap->samples, kFrames - 1);
		ASSERT_NE(nullptr, last);
		EXPECT_NEAR(FadeOutFactor(kFrames - 1), last->fade_visibility, 0.03);
	}
}

TEST(motion_track_fade, session_without_fade_detection_stops_on_failures) {
	// Config gate at session level: with fade detection off the same scene
	// drives three consecutive ResidualHigh failures and the arm dies with
	// the historical stop reason.
	FadeScene scene;
	scene.factor = FadeOutFactor;

	FadeSceneReader reader(scene);
	TranslationTrackerConfig config;
	config.fade_detection = false;
	TranslationTrackerBackend backend(config);
	MotionTrackSession session(FadeDomains(), kObject, TrackDirection::Forward,
							   TrackModel::Translation);
	session.SetBackendSeed(0, kObject, SeedCenterX(), SeedCenterY());

	auto lease = VideoProviderLeaseState::Create();
	auto handle = lease->Acquire();
	auto stop = session.RunAnalyze(reader, backend, handle, {});
	EXPECT_EQ(AnalyzeStopReason::ConsecutiveFailures, stop);

	auto snap = session.Capture();
	EXPECT_GE(snap->failure_count, 3);
	bool saw_residual_high = false;
	for (auto const& s : snap->samples)
		if (s.status == TrackStatus::Failed && s.failure == TrackFailureReason::ResidualHigh)
			saw_residual_high = true;
	EXPECT_TRUE(saw_residual_high);
}

TEST(motion_track_fade, high_contrast_template_fades_through_residual_band) {
	// A high-MAD template (clipped ~+-300 swing around the fade level)
	// trips the residual gate from slope ~1 - 22/MAD, well above
	// fade_enter_slope (0.6). Frames in that (0.6, 1 - 22/MAD] band are
	// certified scaled copies; before the ResidualHigh exemption was armed
	// at the earlier threshold they hard-failed for more than
	// kMaxConsecutiveFailures consecutive frames and killed the tracking
	// mid-fade.
	FadeScene scene;
	scene.amplitude = 300.0;
	scene.factor = FadeOutFactor;

	TranslationTrackerBackend backend; // defaults: fade_detection on
	GrayPatch seed;
	FillPatch(scene, 0, kObject, seed);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));
	ASSERT_GT(MedianAbsFromLevel(seed, kFadeLevel), 55.0); // band is real

	double x = SeedCenterX();
	double y = SeedCenterY();
	GrayPatch patch;
	bool saw_band_hold = false;
	for (int frame = 1; frame < kFrames; ++frame) {
		auto step = StepAt(backend, scene, patch, frame, x, y, 12);
		ASSERT_EQ(TrackStatus::Ok, step.status)
			<< "frame " << frame << " reason " << int(step.failure);
		// Held signature (plain steps also measure visibility in the band
		// now, but they publish the ~1.0 NCC as confidence).
		if (IsFadeHeld(step.confidence, step.fade_visibility) && step.fade_visibility > 0.6 && step.fade_visibility < 0.85)
			saw_band_hold = true;
		x = step.candidate_center_x;
		y = step.candidate_center_y;
	}
	// The ramp crossed the band and every frame in it was held, not failed.
	EXPECT_TRUE(saw_band_hold);
}

TEST(motion_track_fade, bidirectional_backward_arm_tracks_backward_ground_truth) {
	// m1 cross-arm leak regression: the session's two arms share ONE
	// backend instance, whose fade state (phase, probe cadence, last
	// accepted center) belongs to the forward arm's side of the seed. The
	// forward arm here ends deep in a fade-hold; without BeginDirectionArm
	// resetting the backend between arms, the backward arm's first steps
	// hold at the FORWARD arm's position until a recovery probe fires.
	// The backward samples must track the backward ground truth instead.
	FadeScene scene;
	// Object drifts one storage px every two frames through the seed
	// (frame 32); the backward ground truth is far from the forward arm's
	// held position.
	scene.offset_x = [](int frame) { return (frame - 32) / 2; };
	// Fade only on the forward side: plateau to frame 40, ramp to the 8%
	// floor by frame 60, constant after.
	scene.factor = [](int frame) {
		if (frame <= 40)
			return 1.0;
		if (frame >= 60)
			return 0.08;
		return 1.0 - 0.92 * (frame - 40) / 20.0;
	};
	constexpr int kSeedFrame = 32;

	FadeSceneReader reader(scene);
	TranslationTrackerBackend backend; // defaults: fade_detection on
	MotionTrackSession session(FadeDomains(), kObject,
							   TrackDirection::Bidirectional,
							   TrackModel::Translation);
	session.SetBackendSeed(kSeedFrame, kObject, SeedCenterX(), SeedCenterY());

	auto lease = VideoProviderLeaseState::Create();
	auto handle = lease->Acquire();
	auto stop = session.RunAnalyze(reader, backend, handle, {});
	EXPECT_EQ(AnalyzeStopReason::Completed, stop);

	auto snap = session.Capture();
	ASSERT_EQ(size_t(kFrames), snap->samples.size());
	for (auto const& s : snap->samples) {
		ASSERT_EQ(TrackStatus::Ok, s.status) << "frame " << s.frame;
		// The backward arm (every frame < seed) and the forward arm's
		// pre-fade frames sit on their own side's ground truth -- never on
		// the opposite arm's coordinates. (Forward frames inside the fade
		// hold the last accepted position by design; the GT check does not
		// apply to them.)
		if (s.frame <= 40) {
			EXPECT_NEAR(scene.GroundCenterX(s.frame), s.center_x, 0.25)
				<< "frame " << s.frame;
			EXPECT_NEAR(scene.GroundCenterY(), s.center_y, 0.25)
				<< "frame " << s.frame;
		}
	}
}

// --- Phase B: session fade interval ----------------------------------------

TEST(motion_track_fade, session_extracts_fade_out_interval) {
	// Plateau, linear fade-out, static faded tail: the published snapshot's
	// fade interval flags fade_out detected with the fitted boundary frames
	// on the analytic ramp (derived like ExpectedResidualCrossing, from the
	// scene's factor shape), and fade_in undetected -- a Forward run has no
	// samples before the seed.
	FadeScene scene;
	scene.factor = LateFadeOutFactor;

	FadeSceneReader reader(scene);
	TranslationTrackerBackend backend; // defaults: fade_detection on
	MotionTrackSession session(FadeDomains(), kObject, TrackDirection::Forward,
							   TrackModel::Translation);
	session.SetBackendSeed(0, kObject, SeedCenterX(), SeedCenterY());

	auto lease = VideoProviderLeaseState::Create();
	auto handle = lease->Acquire();
	auto stop = session.RunAnalyze(reader, backend, handle, {});
	EXPECT_EQ(AnalyzeStopReason::Completed, stop);

	auto snap = session.Capture();
	ASSERT_EQ(size_t(kFrames), snap->samples.size());
	// Full curve: every Ok sample of the run carries a measurement.
	for (auto const& s : snap->samples)
		EXPECT_GE(s.fade_visibility, 0.0) << "frame " << s.frame;

	EXPECT_FALSE(snap->fade_interval.fade_in.detected);
	auto const& out = snap->fade_interval.fade_out;
	ASSERT_TRUE(out.detected);
	int const expected_full = ExpectedLastFullyVisible(scene);
	int const expected_outer =
		ExpectedFirstVisible(scene, scene.Factor(kFrames - 1));
	EXPECT_NEAR(double(expected_full), out.full_visibility_frame, 1.0)
		<< "full " << out.full_visibility_frame;
	EXPECT_LE(expected_full + 2, out.outer_frame);
	EXPECT_NEAR(double(expected_outer), out.outer_frame, 2.0)
		<< "outer " << out.outer_frame;
	EXPECT_GT(out.confidence, 0.0);
}

TEST(motion_track_fade, session_without_fade_reports_no_interval) {
	// Static unfaded scene: the full curve sits at 1.0 everywhere and the
	// interval stays undetected in both directions -- the plateau reaches
	// the end of the observed curve, so there is no ramp to fit.
	FadeScene scene;
	scene.factor = [](int) { return 1.0; };

	FadeSceneReader reader(scene);
	TranslationTrackerBackend backend; // defaults: fade_detection on
	MotionTrackSession session(FadeDomains(), kObject, TrackDirection::Forward,
							   TrackModel::Translation);
	session.SetBackendSeed(0, kObject, SeedCenterX(), SeedCenterY());

	auto lease = VideoProviderLeaseState::Create();
	auto handle = lease->Acquire();
	auto stop = session.RunAnalyze(reader, backend, handle, {});
	EXPECT_EQ(AnalyzeStopReason::Completed, stop);

	auto snap = session.Capture();
	ASSERT_EQ(size_t(kFrames), snap->samples.size());
	for (auto const& s : snap->samples)
		EXPECT_NEAR(1.0, s.fade_visibility, 1e-6) << "frame " << s.frame;
	EXPECT_FALSE(snap->fade_interval.fade_in.detected);
	EXPECT_FALSE(snap->fade_interval.fade_out.detected);
}

// --- Phase B: apply-plan \fad composition ----------------------------------

namespace {

struct ApplyFixture {
	AssFile file;
	std::vector<AssDialogue *> lines;

	ApplyFixture() {
		auto *style = new AssStyle; // Default: outline_w 2, shadow_w 2
		style->name = "Default";
		file.Styles.push_back(*style);
	}

	AssDialogue *AddLine(int start_ms, int end_ms, std::string text) {
		auto *line = new AssDialogue;
		line->Start = start_ms;
		line->End = end_ms;
		line->Text = std::move(text);
		file.Events.push_back(*line);
		lines.push_back(line);
		return line;
	}
};

ApplyPlanInput FadeApplyBaseInput() {
	ApplyPlanInput in;
	in.model = TrackModel::Translation;
	in.decode_interval = FrameInterval{0, 19};
	in.direction_domain = FrameInterval{0, 19};
	in.storage_width = 1920;
	in.storage_height = 1080;
	in.script_width = 1920;
	in.script_height = 1080;
	in.timecodes = agi::vfr::Framerate(10.0); // 100 ms frames
	in.video_frame_count = 20;
	in.seed_time_ms = 0;
	in.options.mode = ApplyMode::Exact;
	return in;
}

// Ok samples at a storage-pixel center; per_frame_dx shifts the center 1
// storage px per frame for the moving-trajectory cases.
void AddSamples(ApplyPlanInput& in, double x, double y,
				double per_frame_dx = 0.0) {
	for (int f = 0; f <= in.decode_interval.last; ++f) {
		TrackSample s;
		s.frame = f;
		s.model = TrackModel::Translation;
		s.status = TrackStatus::Ok;
		s.confidence = 1.0;
		s.center_x = x + per_frame_dx * f;
		s.center_y = y;
		s.fade_visibility = 1.0;
		in.samples.push_back(s);
	}
}

FadeInterval FadeOutOnlyInterval(int outer_frame, int full_visibility_frame) {
	FadeInterval interval;
	interval.fade_out.detected = true;
	interval.fade_out.outer_frame = outer_frame;
	interval.fade_out.full_visibility_frame = full_visibility_frame;
	return interval;
}

FadeInterval FadeInOnlyInterval(int outer_frame, int full_visibility_frame) {
	FadeInterval interval;
	interval.fade_in.detected = true;
	interval.fade_in.outer_frame = outer_frame;
	interval.fade_in.full_visibility_frame = full_visibility_frame;
	return interval;
}

// Parses the first "\fad(in,out)"; false when absent.
bool ParseFad(std::string const& text, int& out_in, int& out_out) {
	std::string const needle = "\\fad(";
	auto const at = text.find(needle);
	if (at == std::string::npos)
		return false;
	int fade_in = 0;
	int fade_out = 0;
	if (std::sscanf(text.c_str() + at + needle.size(), "%d,%d", &fade_in,
					&fade_out) != 2)
		return false;
	out_in = fade_in;
	out_out = fade_out;
	return true;
}

// Parses the first "\fade(a1,a2,a3,t1,t2,t3,t4)"; false when absent.
bool ParseFade7(std::string const& text, int& a1, int& a2, int& a3,
				int& t1, int& t2, int& t3, int& t4) {
	std::string const needle = "\\fade(";
	auto const at = text.find(needle);
	if (at == std::string::npos)
		return false;
	return std::sscanf(text.c_str() + at + needle.size(),
					   "%d,%d,%d,%d,%d,%d,%d", &a1, &a2, &a3, &t1, &t2, &t3,
					   &t4) == 7;
}

size_t CountOccurrences(std::string const& text, std::string const& needle) {
	size_t count = 0;
	size_t at = text.find(needle);
	while (at != std::string::npos) {
		++count;
		at = text.find(needle, at + 1);
	}
	return count;
}

} // namespace

TEST(motion_track_fade, apply_plan_composes_fad_with_exact_expected_times) {
	// Exact hand-derived values at 10 fps (EXACT(f) = 100f, START(f) =
	// 100f - 50, END(f) = 100f + 50). The line-anchored convention (M2 +
	// C1): the fade-out ramp starts at END(full_out) = END(10) = 1050 and
	// completes at the LINE's End, clamped and centisecond-rounded.
	//  - Line [0, 1950] ends exactly at END(19) = dom_end, so the whole
	//    line is one covered part whose local window [0, 1950] covers the
	//    entire curve: fade_out = 1950 - 1050 = 900 and the tag collapses
	//    to the simple full-curve form \fad(0,900).
	//  - The offset line [500, 1500] is shorter than and offset from the
	//    ramp: fade_out = clamp(1500 - 1050) = 450, its covered part
	//    [500, 1450] starts mid-plateau and ends mid-ramp, so the 7-arg
	//    slice is \fade(255,0,227,0,0,550,950): a1 is the normalized
	//    no-fade-in endpoint (t2 = 0 renders nothing before it), a3 =
	//    round(255 * (1450 - 1050) / 450) = 227 at the part end, t3 =
	//    1050 - 500 = 550 and t4 = 950 are the ramp breakpoints clamped
	//    into the window. The uncovered suffix [1450, 1500] keeps the
	//    source text verbatim (nothing stale to strip) and gains no tag.
	//  - The both-fades line [0, 1950]: fade_in = START(3) = 250,
	//    fade_out = 1950 - END(12) = 1950 - 1250 = 700 -> \fad(250,700).
	ApplyFixture fx;
	auto *line = fx.AddLine(0, 1950, R"({\pos(100,100)}x)");
	auto *offset = fx.AddLine(500, 1500, R"({\pos(100,100)}x)");
	auto *both = fx.AddLine(0, 1950, R"({\pos(100,100)}x)");
	auto input = FadeApplyBaseInput();
	AddSamples(input, 100, 100);
	input.origin_center_x = 100;
	input.origin_center_y = 100;
	input.options.apply_fad = true;

	input.fade_interval = FadeOutOnlyInterval(15, 10);
	auto plan = BuildApplyPlan(fx.file, {line, offset}, input);
	ASSERT_TRUE(plan.has_mutations());
	ASSERT_EQ(size_t(2), plan.lines.size());
	ASSERT_EQ(size_t(1), plan.lines[0].parts.size());
	EXPECT_EQ(R"({\fad(0,900)\pos(100.00,100.00)}x)",
			  plan.lines[0].parts.front().text);
	int fade_in = -1;
	int fade_out = -1;
	ASSERT_TRUE(ParseFad(plan.lines[0].parts.front().text, fade_in, fade_out));
	EXPECT_EQ(0, fade_in);
	EXPECT_EQ(900, fade_out);

	ASSERT_EQ(size_t(2), plan.lines[1].parts.size()); // covered + suffix
	EXPECT_TRUE(plan.lines[1].parts[0].covered);
	EXPECT_EQ(R"({\fade(255,0,227,0,0,550,950)\pos(100.00,100.00)}x)",
			  plan.lines[1].parts[0].text);
	EXPECT_FALSE(plan.lines[1].parts[1].covered);
	EXPECT_EQ(R"({\pos(100,100)}x)", plan.lines[1].parts[1].text);

	input.fade_interval = FadeInterval{};
	input.fade_interval->fade_in = FadeInOnlyInterval(0, 3).fade_in;
	input.fade_interval->fade_out = FadeOutOnlyInterval(15, 12).fade_out;
	plan = BuildApplyPlan(fx.file, {both}, input);
	ASSERT_TRUE(plan.has_mutations());
	ASSERT_EQ(size_t(1), plan.lines.size());
	ASSERT_EQ(size_t(1), plan.lines[0].parts.size());
	EXPECT_EQ(R"({\fad(250,700)\pos(100.00,100.00)}x)",
			  plan.lines[0].parts.front().text);
}

TEST(motion_track_fade, apply_plan_fad_option_off_is_byte_identical) {
	// Option off: the interval is ignored and the output matches the
	// pre-feature planner exactly -- no \fad anywhere in any part.
	ApplyFixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)}x)");
	auto input = FadeApplyBaseInput();
	AddSamples(input, 100, 100);
	input.origin_center_x = 100;
	input.origin_center_y = 100;
	input.fade_interval = FadeOutOnlyInterval(15, 10);
	// options.apply_fad defaults to false.

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	for (auto const& part : plan.lines[0].parts)
		EXPECT_EQ(std::string::npos, part.text.find("\\fad"))
			<< part.text;
	EXPECT_EQ(R"({\pos(100.00,100.00)}x)", plan.lines[0].parts.front().text);
}

TEST(motion_track_fade, apply_plan_without_interval_emits_no_fad) {
	// Option on but no fade data (nullopt, or an all-undetected interval):
	// the composition is skipped and the output stays byte-identical.
	ApplyFixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)}x)");
	auto input = FadeApplyBaseInput();
	AddSamples(input, 100, 100);
	input.origin_center_x = 100;
	input.origin_center_y = 100;
	input.options.apply_fad = true;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	EXPECT_EQ(R"({\pos(100.00,100.00)}x)", plan.lines[0].parts.front().text);

	input.fade_interval = FadeInterval{}; // present, nothing detected
	plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	EXPECT_EQ(R"({\pos(100.00,100.00)}x)", plan.lines[0].parts.front().text);
	for (auto const& part : plan.lines[0].parts)
		EXPECT_EQ(std::string::npos, part.text.find("\\fad")) << part.text;
}

TEST(motion_track_fade, apply_plan_replaces_existing_fad_not_duplicates) {
	// C1 per-part slicing on a multi-part line: the moving trajectory
	// splits into one Exact part per frame (part k spans [100k-50,
	// 100k+50]; part 0 = [0,50], part 19 ends at dom_end = END(19) =
	// 1950). Line [0, 2000], fade-out anchor END(10) = 1050, so the ramp
	// is [1050, 2000] and fade_out = 950. Parts whose window ends at or
	// before 1050 lie entirely in the plateau and carry NO tag; each ramp
	// part carries the exact \fade reproducing the global curve
	// alpha(t) = round(255 * (t - 1050) / 950) over its own 100 ms window
	// (t3 = 0: the whole window sits inside the fade-out ramp, so the
	// rendered ramp runs a2 -> a3 with a2 = alpha(part_start)). The stale
	// \fad(500,500) is gone everywhere -- the representation exists as
	// per-part slices, never duplicated.
	ApplyFixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\fad(500,500)\pos(100,100)}x)");
	auto input = FadeApplyBaseInput();
	AddSamples(input, 100, 100, /*per_frame_dx=*/1.0); // Exact: one part/frame
	input.origin_center_x = 100;
	input.origin_center_y = 100;
	input.fade_interval = FadeOutOnlyInterval(15, 10);
	input.options.apply_fad = true;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	auto const& parts = plan.lines[0].parts;
	ASSERT_EQ(size_t(21), parts.size()); // 20 covered + uncovered suffix
	auto const ramp_alpha = [](int t) {
		return int(std::lround(255.0 * (t - 1050) / 950.0));
	};
	char pos[32];
	for (int k = 0; k < 20; ++k) {
		ASSERT_TRUE(parts[size_t(k)].covered) << "part " << k;
		std::snprintf(pos, sizeof(pos), "%d.00", 100 + k);
		int const start = k == 0 ? 0 : 100 * k - 50;
		int const end = 100 * k + 50;
		std::string expected = "{\\pos(" + std::string(pos) + ",100.00)}x";
		if (end > 1050) {
			expected = "{\\fade(255," + std::to_string(ramp_alpha(start)) + "," + std::to_string(ramp_alpha(end)) + ",0,0,0,100)" + expected.substr(1);
		}
		EXPECT_EQ(expected, parts[size_t(k)].text) << "part " << k;
	}
	// Uncovered suffix [1950, 2000]: stale tag stripped, no new tag.
	EXPECT_FALSE(parts[20].covered);
	EXPECT_EQ(R"({\pos(100,100)}x)", parts[20].text);
	// No part carries a 2-arg \fad; the slices are 7-arg \fade tags.
	for (size_t i = 0; i < parts.size(); ++i)
		EXPECT_EQ(size_t(0), CountOccurrences(parts[i].text, "\\fad("))
			<< "part " << i << " " << parts[i].text;
}

TEST(motion_track_fade, apply_plan_fade_in_slices_parts_starting_mid_ramp) {
	// The fade-in direction of the per-part slicing: anchor START(5) =
	// 450, line [0, 2000] -> fade_in = 450, ramp [0, 450]. Every early
	// part starts MID-ramp and ends mid-ramp: t1 = 0 with a1 already
	// mid-ramp, t2 clamps to the part's own end (the whole window is
	// inside the ramp, so the rendered a1 -> a2 interpolation must end at
	// the curve's own alpha at the part end: a2 = a3 = alpha(end)), and
	// a3 is the normalized no-fade-out endpoint (t3 = t4 = dur renders
	// nothing after it). Parts past the ramp are plateau-only and carry
	// no tag. alpha(t) = round(255 * (1 - t / 450)).
	ApplyFixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)}x)");
	auto input = FadeApplyBaseInput();
	AddSamples(input, 100, 100, /*per_frame_dx=*/1.0);
	input.origin_center_x = 100;
	input.origin_center_y = 100;
	input.fade_interval = FadeInOnlyInterval(0, 5);
	input.options.apply_fad = true;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	auto const& parts = plan.lines[0].parts;
	ASSERT_EQ(size_t(21), parts.size());
	auto const ramp_alpha = [](int t) {
		return int(std::lround(255.0 * (1.0 - double(t) / 450.0)));
	};
	char pos[32];
	for (int k = 0; k < 20; ++k) {
		std::snprintf(pos, sizeof(pos), "%d.00", 100 + k);
		int const start = k == 0 ? 0 : 100 * k - 50;
		int const end = 100 * k + 50;
		std::string expected = "{\\pos(" + std::string(pos) + ",100.00)}x";
		if (start < 450) {
			expected = "{\\fade(" + std::to_string(ramp_alpha(start)) + "," + std::to_string(ramp_alpha(end)) + ",255,0," + std::to_string(end - start) + "," + std::to_string(end - start) + "," + std::to_string(end - start) + ")" + expected.substr(1);
		}
		EXPECT_EQ(expected, parts[size_t(k)].text) << "part " << k;
	}
	// Plateau-only parts (frames past the ramp) carry no tag at all.
	for (int k = 5; k < 20; ++k)
		EXPECT_EQ(std::string::npos, parts[size_t(k)].text.find("\\fade"))
			<< "part " << k << " " << parts[size_t(k)].text;
}

TEST(motion_track_fade, apply_plan_alpha_transform_source_replaced_per_part) {
	// M3: the source line's fade is the leading \alpha + \t representation
	// -- exactly what ApplyAssFade would claim on a single event. No part
	// may replay the original animation: the claimable representation is
	// stripped everywhere (align_video_fade::StripClaimableFade shares the
	// predicate), covered parts carry their own per-part slice instead,
	// and the uncovered suffix keeps only the non-fade tags verbatim.
	ApplyFixture fx;
	auto *line = fx.AddLine(
		0, 2000, R"({\alpha&HFF&\t(0,700,\alpha&H00&)\pos(100,100)}x)");
	auto input = FadeApplyBaseInput();
	AddSamples(input, 100, 100, /*per_frame_dx=*/1.0); // Exact: one part/frame
	input.origin_center_x = 100;
	input.origin_center_y = 100;
	input.fade_interval = FadeOutOnlyInterval(15, 10);
	input.options.apply_fad = true;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	auto const& parts = plan.lines[0].parts;
	ASSERT_EQ(size_t(21), parts.size());
	size_t tagged = 0;
	for (size_t i = 0; i < parts.size(); ++i) {
		// The original animation never survives on any part.
		EXPECT_EQ(size_t(0), CountOccurrences(parts[i].text, "\\alpha"))
			<< "part " << i << " " << parts[i].text;
		EXPECT_EQ(size_t(0), CountOccurrences(parts[i].text, "\\t("))
			<< "part " << i << " " << parts[i].text;
		if (parts[i].text.find("\\fade(") != std::string::npos)
			++tagged;
		EXPECT_EQ(size_t(0), CountOccurrences(parts[i].text, "\\fad("))
			<< "part " << i << " " << parts[i].text;
	}
	// The ramp parts ([1050, 2000) at 100 ms each) each carry a slice;
	// plateau parts and the uncovered suffix carry none.
	EXPECT_EQ(size_t(9), tagged);
	EXPECT_EQ(R"({\fade(255,0,27,0,0,0,100)\pos(111.00,100.00)}x)",
			  parts[11].text);
	EXPECT_EQ(R"({\fade(255,215,242,0,0,0,100)\pos(119.00,100.00)}x)",
			  parts[19].text);
	// Uncovered suffix: the stale \alpha + \t animation is stripped, the
	// position tag survives verbatim, no new tag is added.
	EXPECT_FALSE(parts[20].covered);
	EXPECT_EQ(R"({\pos(100,100)}x)", parts[20].text);
}

TEST(motion_track_fade, apply_plan_fad_is_mode_agnostic) {
	// Documented choice: the option composes per-part fades in Compact
	// mode too (the fade describes the event, not the trajectory fit). A
	// linear trajectory collapses to a single \move part [0, dom_end =
	// 1950]; the fade-out ramp [1050, 2000] ends past the part's window,
	// so the part ends mid-ramp: \fade(255,0,242,0,0,1050,1950) with
	// a3 = round(255 * (1950 - 1050) / 950) = 242 (M2: the duration is
	// anchored on the LINE's End = 2000, past the tracked domain).
	ApplyFixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)}x)");
	auto input = FadeApplyBaseInput();
	input.options.mode = ApplyMode::Compact;
	AddSamples(input, 100, 100, /*per_frame_dx=*/1.0);
	input.origin_center_x = 100;
	input.origin_center_y = 100;
	input.fade_interval = FadeOutOnlyInterval(15, 10);
	input.options.apply_fad = true;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	auto const& parts = plan.lines[0].parts;
	ASSERT_EQ(size_t(2), parts.size()); // covered \move part + suffix
	EXPECT_TRUE(parts[0].covered);
	EXPECT_EQ(
		R"({\fade(255,0,242,0,0,1050,1950)\move(100.00,100.00,119.00,100.00,0,1900)}x)",
		parts[0].text);
	EXPECT_FALSE(parts[1].covered);
	EXPECT_EQ(R"({\pos(100,100)}x)", parts[1].text);
}

TEST(motion_track_fade, session_interval_flows_into_apply_plan_fad) {
	// End to end: the session's fitted frames feed ApplyPlanInput, and
	// every emitted part carries the \fade slice of the line-anchored
	// curve: the fade-out ramp starts at END(full_visibility_frame) and
	// runs to the line's own End (M2), so each covered part's tag is
	// derived analytically from its own window and that curve.
	FadeScene scene;
	scene.factor = LateFadeOutFactor;

	FadeSceneReader reader(scene);
	TranslationTrackerBackend backend; // defaults: fade_detection on
	MotionTrackSession session(FadeDomains(), kObject, TrackDirection::Forward,
							   TrackModel::Translation);
	session.SetBackendSeed(0, kObject, SeedCenterX(), SeedCenterY());
	auto lease = VideoProviderLeaseState::Create();
	auto handle = lease->Acquire();
	ASSERT_EQ(AnalyzeStopReason::Completed,
			  session.RunAnalyze(reader, backend, handle, {}));
	auto snap = session.Capture();
	ASSERT_TRUE(snap->fade_interval.fade_out.detected);
	// LateFadeOutFactor: the plateau ends at frame 20 (factor(21) is the
	// first below the fully-visible band), so the anchor is END(20).
	EXPECT_NEAR(20.0, snap->fade_interval.fade_out.full_visibility_frame, 1.0);

	ApplyFixture fx;
	auto *line = fx.AddLine(0, kFrames * 100, R"({\pos(43.5,29.5)}x)");
	auto input = FadeApplyBaseInput();
	FillApplyInputFromSnapshot(input, *snap);
	ASSERT_TRUE(input.fade_interval);
	EXPECT_TRUE(input.fade_interval->fade_out.detected);
	input.storage_width = FadeScene::kWidth;
	input.storage_height = FadeScene::kHeight;
	input.script_width = FadeScene::kWidth;
	input.script_height = FadeScene::kHeight;
	input.video_frame_count = kFrames;
	input.origin_center_x = SeedCenterX();
	input.origin_center_y = SeedCenterY();
	input.options.apply_fad = true;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	auto const& parts = plan.lines[0].parts;
	ASSERT_GT(parts.size(), 1u);

	// The planner's line-anchored convention, replicated: the fade-out
	// ramp starts at END(full_visibility) and completes at the line's End.
	int const line_end = kFrames * 100;
	auto const& interval = snap->fade_interval;
	auto const line_timing =
		aegisub::align_video_fade::RoundAssFadeTimingToCentiseconds(
			aegisub::align_video_fade::AssFadeTiming{
				0, line_end, 0,
				line_end - input.timecodes.TimeAtFrame(
							   interval.fade_out.full_visibility_frame,
							   agi::vfr::Time::END)});
	ASSERT_GT(line_timing.fade_out_ms, 4000); // ramp to the line end
	ASSERT_LT(line_timing.fade_out_ms, 4400);
	int const ramp_out_start = line_end - line_timing.fade_out_ms;
	auto const curve_alpha = [&](int t) {
		double const visibility =
			t <= ramp_out_start ? 1.0
								: double(line_end - t) / line_timing.fade_out_ms;
		return int(std::lround((1.0 - visibility) * 255.0));
	};

	for (auto const& part : parts) {
		if (!part.covered) {
			// Uncovered suffix past the tracked domain: no fade tag.
			EXPECT_EQ(std::string::npos, part.text.find("\\fad")) << part.text;
			EXPECT_EQ(std::string::npos, part.text.find("\\fade")) << part.text;
			continue;
		}
		int const dur = part.end_ms - part.start_ms;
		if (part.end_ms <= ramp_out_start) {
			// Plateau-only part: no tag.
			EXPECT_EQ(std::string::npos, part.text.find("\\fade"))
				<< part.text;
			continue;
		}
		// Ramp part: a1 is the normalized no-fade-in endpoint (every part
		// here starts at or after the plateau), a3 is the curve's alpha at
		// the part end (the last covered part ends at dom_end = END(63) =
		// 6350, mid-ramp), t3 the clamped ramp start offset, t4 the
		// duration; a2 continues the curve from a1 when the whole window
		// sits inside the ramp, else the plateau level 0.
		int a1 = 0, a2 = 0, a3 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;
		ASSERT_TRUE(ParseFade7(part.text, a1, a2, a3, t1, t2, t3, t4))
			<< part.text;
		EXPECT_EQ(255, a1) << part.text;
		EXPECT_EQ(0, t1) << part.text;
		EXPECT_EQ(0, t2) << part.text;
		EXPECT_EQ(std::clamp(ramp_out_start - part.start_ms, 0, dur), t3)
			<< part.text;
		EXPECT_EQ(dur, t4) << part.text;
		if (t3 == 0)
			EXPECT_EQ(curve_alpha(part.start_ms), a2) << part.text;
		else
			EXPECT_EQ(0, a2) << part.text;
		EXPECT_EQ(curve_alpha(part.end_ms), a3) << part.text;
	}
}
