#include <main.h>

#include "../../src/motion_track/session.h"
#include "../../src/motion_track/synthetic_frame_reader.h"
#include "../../src/motion_track/translation_backend.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace {
using namespace aegisub::motion_track;

RoiRect const kObject{40, 30, 24, 16};
double const kVx = 5.0;
constexpr double kDeg = 3.14159265358979323846 / 180.0;

SyntheticTranslationScene MakeScene(double vx, double vy) {
	SyntheticTranslationScene::Config config;
	config.width = 320;
	config.height = 240;
	config.frame_count = 40;
	config.object_at_frame0 = kObject;
	config.velocity_x = vx;
	config.velocity_y = vy;
	return SyntheticTranslationScene(config);
}

SessionDomains Domains(int first, int last) {
	SessionDomains d;
	d.decode_interval = FrameInterval{0, 39};
	d.direction_domain = FrameInterval{first, last};
	d.video_frame_count = 40;
	d.search_radius_override = 14;
	return d;
}

double CenterX(int frame) { return 40 + 11.5 + kVx * frame; }
double CenterY() { return 30 + 7.5; }

struct FlatAfterReader final : MotionFrameReader {
	MotionFrameReader& base;
	int flat_from;
	explicit FlatAfterReader(MotionFrameReader& base, int flat_from)
		: base(base), flat_from(flat_from) {}

	FrameReadResult FetchGray(int frame, RoiRect roi, GrayPatch& out) override {
		if (frame >= flat_from) {
			out.frame = frame;
			out.origin_x = roi.x;
			out.origin_y = roi.y;
			out.width = std::max(roi.w, 0);
			out.height = std::max(roi.h, 0);
			out.stride = out.width;
			out.gray.assign(size_t(out.width) * size_t(out.height), 77);
			return FrameReadResult{FrameReadStatus::Ok, {}};
		}
		return base.FetchGray(frame, roi, out);
	}
};

std::shared_ptr<VideoProviderLeaseState> FreshLease() {
	return VideoProviderLeaseState::Create();
}

// Always-Ok reader handing back constant-gray crops; enough for backends
// that never look at the pixels.
struct FlatReader final : MotionFrameReader {
	FrameReadResult FetchGray(int frame, RoiRect roi, GrayPatch& out) override {
		out.frame = frame;
		out.origin_x = roi.x;
		out.origin_y = roi.y;
		out.width = std::max(roi.w, 0);
		out.height = std::max(roi.h, 0);
		out.stride = out.width;
		out.gray.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height), 77);
		return FrameReadResult{.status = FrameReadStatus::Ok};
	}
};

double SampleRotation(TrackSample const& s) {
	return std::atan2(s.transform.matrix[3], s.transform.matrix[0]);
}

// Scripted similarity backend: after every Reset, each Step reports a pure
// rotation of (frame - reset frame) * step_rotation -- template-relative,
// i.e. identity at the reset frame -- at a center drifting along +x. The
// session-level composition of a reseed base against these scripted
// increments is then exact (R(a)·R(b) = R(a+b)).
class ScriptedSimilarityBackend final : public TrackerBackend {
	public:
	double step_rotation = 0.0; // radians per stepped frame

	TrackModel Model() const override { return TrackModel::Similarity; }
	std::string_view Name() const noexcept override {
		return "scripted-similarity";
	}

	TrackStatus Reset(TrackerSeed const& seed) override {
		if (seed.model != TrackModel::Similarity)
			return TrackStatus::Unsupported;
		reset_frame_ = seed.seed_frame;
		return TrackStatus::Ok;
	}

	TrackStepResult Step(TrackStepRequest const& request) override {
		TrackStepResult result;
		result.frame = request.frame;
		result.status = TrackStatus::Ok;
		result.confidence = 1.0;
		double const angle = (request.frame - reset_frame_) * step_rotation;
		double const c = std::cos(angle);
		double const s = std::sin(angle);
		// Column-vector layout: [m00 m01; m10 m11] = R(angle).
		result.transform.matrix[0] = c;
		result.transform.matrix[1] = -s;
		result.transform.matrix[3] = s;
		result.transform.matrix[4] = c;
		result.candidate_center_x = 100.0 + 2.0 * request.frame;
		result.candidate_center_y = 80.0;
		return result;
	}

	private:
	int reset_frame_ = -1;
};
}

TEST(motion_track_session, forward_run_completes_with_analytic_centers) {
	auto scene = MakeScene(kVx, 0.0);
	SyntheticFrameReader reader(scene);
	TranslationTrackerBackend backend;

	MotionTrackSession session(Domains(0, 39), kObject, TrackDirection::Forward, TrackModel::Translation);
	session.SetBackendSeed(0, kObject, CenterX(0), CenterY());

	auto lease = VideoProviderLeaseState::Create();
	auto handle = lease->Acquire();
	auto stop = session.RunAnalyze(reader, backend, handle, {});
	EXPECT_EQ(AnalyzeStopReason::Completed, stop);

	auto snap = session.Capture();
	ASSERT_EQ(snap->samples.size(), size_t(40));
	for (int f : {0, 10, 25, 39}) {
		auto const *s = FindSample(snap->samples, f);
		ASSERT_NE(nullptr, s);
		EXPECT_NEAR(CenterX(f), s->center_x, 0.25) << "frame " << f;
	}
	// Origin-relative published transform.
	auto const *last = FindSample(snap->samples, 39);
	EXPECT_NEAR(last->transform.matrix[2],
				last->center_x - snap->origin_center_x, 1e-9);
}

TEST(motion_track_session,
	 continue_at_frame_11_replaces_seed_and_keeps_centers_continuous) {
	auto scene = MakeScene(kVx, 0.0);
	SyntheticFrameReader reader(scene);
	TranslationTrackerBackend backend;

	// First pass: track 0..10 then cancel (frames 11..39 stay Missing).
	MotionTrackSession session(Domains(0, 39), kObject, TrackDirection::Forward, TrackModel::Translation);
	session.SetBackendSeed(0, kObject, CenterX(0), CenterY());
	int done = 0;
	auto lease = VideoProviderLeaseState::Create();
	auto handle = lease->Acquire();
	auto stop = session.RunAnalyze(
		reader, backend, handle,
		{[&] { return done >= 11; }, [&](int d, int) { done = d; }});
	EXPECT_EQ(AnalyzeStopReason::UserCanceled, stop);

	auto snap = session.Capture();
	auto const *s10 = FindSample(snap->samples, 10);
	ASSERT_NE(nullptr, s10);
	if (auto const *s12 = FindSample(snap->samples, 12))
		EXPECT_EQ(TrackStatus::Missing, s12->status);
	std::uint64_t const revision_before = snap->revision;

	// Continue: reseed at frame 11 near the analytic center, rerun to the end.
	session.SetBackendSeed(11, RoiRect{40 + int(5 * 11), 30, 24, 16},
						   CenterX(11), CenterY());
	handle = lease->Acquire();
	stop = session.RunAnalyze(reader, backend, handle, {});
	EXPECT_EQ(AnalyzeStopReason::Completed, stop);

	snap = session.Capture();
	EXPECT_GT(snap->revision, revision_before);
	EXPECT_EQ(snap->origin_seed_frame, 0); // Continue never moves the origin

	auto const *seed_sample = FindSample(snap->samples, 11);
	ASSERT_NE(nullptr, seed_sample);
	EXPECT_EQ(TrackStatus::Ok, seed_sample->status);

	auto const *prev = FindSample(snap->samples, 10);
	auto const *next = FindSample(snap->samples, 12);
	ASSERT_NE(nullptr, prev);
	ASSERT_NE(nullptr, next);
	EXPECT_NEAR(seed_sample->center_x - prev->center_x, 5.0, 0.25);
	EXPECT_NEAR(next->center_x - seed_sample->center_x, 5.0, 0.25);
}

TEST(motion_track_session, consecutive_flat_frames_stop_the_direction) {
	auto scene = MakeScene(kVx, 0.0);
	SyntheticFrameReader reader(scene);
	FlatAfterReader flat_after(reader, 25);
	TranslationTrackerBackend backend;

	MotionTrackSession session(Domains(0, 39), kObject, TrackDirection::Forward, TrackModel::Translation);
	session.SetBackendSeed(0, kObject, CenterX(0), CenterY());

	auto lease = VideoProviderLeaseState::Create();
	auto handle = lease->Acquire();
	auto stop = session.RunAnalyze(
		flat_after, backend, handle,
		{});
	EXPECT_EQ(AnalyzeStopReason::ConsecutiveFailures, stop);

	auto snap = session.Capture();
	for (int f = 28; f <= 39; ++f) {
		auto const *s = FindSample(snap->samples, f);
		if (s)
			EXPECT_EQ(TrackStatus::Missing, s->status) << "frame " << f;
	}
	auto const *ok26 = FindSample(snap->samples, 24);
	EXPECT_NE(nullptr, ok26);
}

TEST(motion_track_session, range_caps_match_documented_thresholds) {
	EXPECT_EQ(RangeCheck::Ok, CheckDecodeInterval(FrameInterval{0, 39}));
	EXPECT_EQ(RangeCheck::NeedsConfirmation, CheckDecodeInterval(FrameInterval{0, 1501}));
	EXPECT_EQ(RangeCheck::TooLong, CheckDecodeInterval(FrameInterval{0, 10001}));

	MotionTrackSession ok(Domains(0, 39), kObject, TrackDirection::Forward, TrackModel::Translation);
	EXPECT_EQ(RangeCheck::Ok, ok.CheckRange());

	SessionDomains big = Domains(0, 39);
	big.decode_interval = FrameInterval{0, 1501};
	MotionTrackSession confirm(big, kObject, TrackDirection::Forward, TrackModel::Translation);
	EXPECT_EQ(RangeCheck::NeedsConfirmation, confirm.CheckRange());

	SessionDomains huge = Domains(0, 39);
	huge.decode_interval = FrameInterval{0, 10001};
	MotionTrackSession reject(huge, kObject, TrackDirection::Forward, TrackModel::Translation);
	EXPECT_EQ(RangeCheck::TooLong, reject.CheckRange());

	// A TooLong domain must abort the run before touching any frames.
	auto scene = MakeScene(0.0, 0.0);
	SyntheticFrameReader reader(scene);
	TranslationTrackerBackend backend;
	SessionDomains reject2 = Domains(0, 39);
	reject2.decode_interval = FrameInterval{0, 10001};
	MotionTrackSession session(reject2, kObject, TrackDirection::Forward, TrackModel::Translation);
	session.SetBackendSeed(0, kObject, CenterX(0), CenterY());
	auto lease_too = VideoProviderLeaseState::Create();
	auto handle_too = lease_too->Acquire();
	EXPECT_EQ(AnalyzeStopReason::Error,
			  session.RunAnalyze(reader, backend, handle_too, {}));
}

TEST(motion_track_session, provider_retirement_invalidates_the_run) {
	auto scene = MakeScene(3.0, 0.0);
	SyntheticFrameReader reader(scene);
	TranslationTrackerBackend backend;
	auto lease_state = VideoProviderLeaseState::Create();

	MotionTrackSession session(Domains(0, 39), kObject, TrackDirection::Forward, TrackModel::Translation);
	session.SetBackendSeed(0, kObject, CenterX(0), CenterY());

	lease_state->Begin();                 // retire before the run starts
	auto handle = lease_state->Acquire(); // fails; still reports changed
	EXPECT_FALSE(handle);
	auto stop = session.RunAnalyze(reader, backend, handle, {});

	auto snap = session.Capture();
	EXPECT_TRUE(snap->samples.empty());
}

TEST(motion_track_session, bidirectional_tracks_both_arms) {
	auto scene = MakeScene(kVx, 0.0);
	SyntheticFrameReader reader(scene);
	TranslationTrackerBackend backend;

	MotionTrackSession session(Domains(0, 39), kObject,
							   TrackDirection::Bidirectional,
							   TrackModel::Translation);
	session.SetBackendSeed(20, RoiRect{40 + int(5 * 20), 30, 24, 16},
						   CenterX(20), CenterY());

	auto lease = VideoProviderLeaseState::Create();
	auto handle = lease->Acquire();
	auto stop = session.RunAnalyze(reader, backend, handle, {});
	EXPECT_EQ(AnalyzeStopReason::Completed, stop);

	auto snap = session.Capture();
	ASSERT_EQ(snap->samples.size(), size_t(40));
	for (int f : {0, 1, 19, 20, 21, 38, 39}) {
		auto const *s = FindSample(snap->samples, f);
		ASSERT_NE(nullptr, s) << "frame " << f;
		EXPECT_EQ(TrackStatus::Ok, s->status) << "frame " << f;
		EXPECT_NEAR(CenterX(f), s->center_x, 0.25) << "frame " << f;
	}
}

TEST(motion_track_session, decode_error_keeps_committed_prefix) {
	auto scene = MakeScene(kVx, 0.0);
	SyntheticFrameReader base(scene);
	struct FailAfterReader final : MotionFrameReader {
		MotionFrameReader& base;
		int fail_from;
		explicit FailAfterReader(MotionFrameReader& b, int f)
			: base(b), fail_from(f) {}
		FrameReadResult FetchGray(int frame, RoiRect roi,
								  GrayPatch& out) override {
			if (frame >= fail_from)
				return FrameReadResult{FrameReadStatus::DecodeError, "boom"};
			return base.FetchGray(frame, roi, out);
		}
	} reader{base, 30};
	TranslationTrackerBackend backend;

	MotionTrackSession session(Domains(0, 39), kObject,
							   TrackDirection::Forward,
							   TrackModel::Translation);
	session.SetBackendSeed(0, kObject, CenterX(0), CenterY());

	auto lease = VideoProviderLeaseState::Create();
	auto handle = lease->Acquire();
	auto stop = session.RunAnalyze(reader, backend, handle, {});
	EXPECT_EQ(AnalyzeStopReason::DecodeError, stop);

	session.Capture(); // sanity: publishable
	auto snap = session.Capture();
	// The committed prefix survives; only the failing frame onward is missing.
	auto const *s10 = FindSample(snap->samples, 10);
	ASSERT_NE(nullptr, s10);
	EXPECT_EQ(TrackStatus::Ok, s10->status);
	auto const *s29 = FindSample(snap->samples, 29);
	ASSERT_NE(nullptr, s29);
	EXPECT_EQ(TrackStatus::Ok, s29->status);
	EXPECT_EQ(nullptr, FindSample(snap->samples, 30));
}

TEST(motion_track_session, run_batch_path_drives_seed_and_steps) {
	auto scene = MakeScene(kVx, 0.0);
	SyntheticFrameReader reader(scene);
	TranslationTrackerBackend backend;

	// Production parity: the plain reader is a stub; every fetch (seed
	// included) must flow through hooks.run_batch.
	struct NullReader final : MotionFrameReader {
		FrameReadResult FetchGray(int, RoiRect, GrayPatch&) override {
			return {FrameReadStatus::Error, "fetching requires run_batch"};
		}
	} null_reader;

	MotionTrackSession session(Domains(0, 20), kObject,
							   TrackDirection::Forward,
							   TrackModel::Translation);
	session.SetBackendSeed(0, kObject, CenterX(0), CenterY());

	auto lease = FreshLease();
	auto handle = lease->Acquire();

	AnalyzeRunHooks hooks;
	int batch_calls = 0;
	hooks.run_batch = [&](std::function<FrameReadResult(
							  MotionFrameReader&)> const& fn) {
		++batch_calls;
		switch (fn(reader).status) {
			case FrameReadStatus::Ok:
				return RawVideoBatchStatus::Completed;
			case FrameReadStatus::ProviderChanged:
				return RawVideoBatchStatus::ProviderChanged;
			case FrameReadStatus::FrameUnavailable:
				return RawVideoBatchStatus::FrameUnavailable;
			default:
				return RawVideoBatchStatus::DecodeError;
		}
	};

	auto stop = session.RunAnalyze(null_reader, backend, handle, hooks);
	EXPECT_EQ(AnalyzeStopReason::Completed, stop);
	EXPECT_GE(batch_calls, 21); // seed + frames 1..20

	auto snap = session.Capture();
	auto const *seed = FindSample(snap->samples, 0);
	ASSERT_NE(nullptr, seed);
	EXPECT_EQ(TrackStatus::Ok, seed->status);
	auto const *last = FindSample(snap->samples, 20);
	ASSERT_NE(nullptr, last);
	EXPECT_EQ(TrackStatus::Ok, last->status);
	EXPECT_NEAR(CenterX(20), last->center_x, 0.25);
}

TEST(motion_track_session, continue_overlap_replaces_stale_samples) {
	auto scene = MakeScene(kVx, 0.0);
	SyntheticFrameReader reader(scene);
	TranslationTrackerBackend backend;

	MotionTrackSession session(Domains(0, 20), kObject,
							   TrackDirection::Forward,
							   TrackModel::Translation);
	// The ROI must sit on the object AS OF the seed frame (it has moved kVx
	// px per frame since frame 0); seeding on the frame-0 footprint would
	// template the static background, whose bumpy correlation surface the
	// ambiguity gate rightly rejects.
	session.SetBackendSeed(10, RoiRect{40 + int(kVx * 10), 30, 24, 16},
						   CenterX(10), CenterY());

	auto lease = FreshLease();
	auto handle = lease->Acquire();
	EXPECT_EQ(AnalyzeStopReason::Completed,
			  session.RunAnalyze(reader, backend, handle, {}));
	auto const after_first = session.Capture()->samples.size(); // frames 10..20

	// Continue from an EARLIER frame: 11..20 get re-tracked; the fresh
	// estimates must replace the committed ones, not duplicate them.
	session.SetBackendSeed(5, RoiRect{40 + int(kVx * 5), 30, 24, 16},
						   CenterX(5), CenterY());
	handle = lease->Acquire();
	EXPECT_EQ(AnalyzeStopReason::Completed,
			  session.RunAnalyze(reader, backend, handle, {}));

	auto snap = session.Capture();
	ASSERT_EQ(snap->samples.size(), after_first + 5); // frames 5..9 added only
	for (int f = 5; f <= 20; ++f) {
		auto const *s = FindSample(snap->samples, f);
		ASSERT_NE(nullptr, s) << "frame " << f;
		EXPECT_EQ(TrackStatus::Ok, s->status) << "frame " << f;
	}
}

TEST(motion_track_session, seed_fetch_preserves_decode_error_stop_reason) {
	auto scene = MakeScene(kVx, 0.0);
	struct SeedFailReader final : MotionFrameReader {
		FrameReadResult FetchGray(int, RoiRect, GrayPatch&) override {
			return {FrameReadStatus::DecodeError, "seed boom"};
		}
	} reader;
	TranslationTrackerBackend backend;
	MotionTrackSession session(Domains(0, 39), kObject, TrackDirection::Forward,
							   TrackModel::Translation);
	session.SetBackendSeed(0, kObject, CenterX(0), CenterY());
	auto lease = VideoProviderLeaseState::Create();
	auto handle = lease->Acquire();
	auto stop = session.RunAnalyze(reader, backend, handle, {});
	EXPECT_EQ(AnalyzeStopReason::DecodeError, stop);
	EXPECT_EQ(AnalyzeStopReason::DecodeError, session.Capture()->stop_reason);
	EXPECT_EQ("seed boom", session.Capture()->diagnostic_message);
}

namespace {
MotionTrackSnapshot SnapshotWithSamples(TrackModel model,
										std::vector<TrackSample> samples) {
	MotionTrackSnapshot snap;
	snap.model = model;
	snap.samples = std::move(samples);
	return snap;
}

TrackSample OkSample(int frame, double cx, double cy) {
	TrackSample s;
	s.frame = frame;
	s.status = TrackStatus::Ok;
	s.center_x = cx;
	s.center_y = cy;
	return s;
}
} // namespace

TEST(motion_track_session, ride_anchor_requires_ok_sample_at_frame) {
	auto snap = SnapshotWithSamples(TrackModel::Translation,
									{OkSample(0, 100, 100), OkSample(10, 200, 150)});
	auto const anchor = RideAnchorAtFrame(snap, 0);
	ASSERT_TRUE(anchor.valid);
	EXPECT_EQ(0, anchor.frame);
	EXPECT_DOUBLE_EQ(100.0, anchor.center_x);
	EXPECT_DOUBLE_EQ(100.0, anchor.center_y);
	// Translation anchors keep the identity linear part.
	EXPECT_DOUBLE_EQ(1.0, anchor.m00);
	EXPECT_DOUBLE_EQ(0.0, anchor.m01);

	// Frame without a sample at all.
	EXPECT_FALSE(RideAnchorAtFrame(snap, 5).valid);
}

TEST(motion_track_session, map_roi_to_frame_rides_translation) {
	auto snap = SnapshotWithSamples(TrackModel::Translation,
									{OkSample(0, 100, 100), OkSample(10, 200, 150)});
	auto const anchor = RideAnchorAtFrame(snap, 0);

	// A box edited at the anchor frame rides the trajectory: the offset the
	// tracked center accumulated by frame 10 carries the rectangle along.
	RoiRect const roi{88, 92, 24, 16};
	RoiRect const mapped = MapRoiToFrame(snap, 10, roi, anchor);
	EXPECT_EQ(188, mapped.x);
	EXPECT_EQ(142, mapped.y);
	EXPECT_EQ(24, mapped.w);
	EXPECT_EQ(16, mapped.h);

	// Without a ride at either end the rectangle passes through unchanged:
	// that is the raw-storage fallback Analyze seeds with.
	EXPECT_EQ(roi.x, MapRoiToFrame(snap, 5, roi, anchor).x);
	EXPECT_EQ(roi.y, MapRoiToFrame(snap, 5, roi, anchor).y);
	RoiRideAnchor invalid;
	EXPECT_EQ(roi.x, MapRoiToFrame(snap, 10, roi, invalid).x);
	EXPECT_EQ(roi.y, MapRoiToFrame(snap, 10, roi, invalid).y);
}

TEST(motion_track_session, map_roi_to_frame_rotates_similarity_bbox) {
	TrackSample rotated = OkSample(5, 150, 120);
	// +90 degrees: (dx, dy) -> (-dy, dx).
	rotated.transform.matrix = {0.0, -1.0, 0.0,
								1.0, 0.0, 0.0,
								0.0, 0.0, 1.0};
	auto snap = SnapshotWithSamples(
		TrackModel::Similarity, {OkSample(0, 100, 100), rotated});
	auto const anchor = RideAnchorAtFrame(snap, 0);

	// 24x16 at the anchor becomes the bounding box of the rotated quad:
	// 16 wide, 24 tall, centered on the frame-5 sample.
	RoiRect const mapped = MapRoiToFrame(snap, 5, RoiRect{88, 92, 24, 16}, anchor);
	EXPECT_EQ(142, mapped.x);
	EXPECT_EQ(108, mapped.y);
	EXPECT_EQ(16, mapped.w);
	EXPECT_EQ(24, mapped.h);
}

TEST(motion_track_session, map_roi_to_frame_encloses_fractional_corners) {
	TrackSample scaled = OkSample(5, 100, 100);
	scaled.transform.matrix = {1.05, 0.0, 0.0,
							   0.0, 1.05, 0.0,
							   0.0, 0.0, 1.0};
	auto snap = SnapshotWithSamples(
		TrackModel::Similarity, {OkSample(0, 100, 100), scaled});
	auto const anchor = RideAnchorAtFrame(snap, 0);

	// Corners at 90 and 110 map to 89.5 and 110.5. Nearest rounding of both
	// extrema would shrink to [90, 110); floor/ceil keeps every source pixel.
	RoiRect const mapped = MapRoiToFrame(snap, 5, RoiRect{90, 90, 20, 20}, anchor);
	EXPECT_EQ(89, mapped.x);
	EXPECT_EQ(89, mapped.y);
	EXPECT_EQ(22, mapped.w);
	EXPECT_EQ(22, mapped.h);
}

TEST(motion_track_session, reseed_roi_anchor_uses_accumulated_linear_part) {
	TrackSample rotated = OkSample(5, 150, 120);
	rotated.transform.matrix = {0.0, -1.0, 0.0,
								1.0, 0.0, 0.0,
								0.0, 0.0, 1.0};
	auto snap = SnapshotWithSamples(
		TrackModel::Similarity, {OkSample(0, 100, 100), rotated});

	auto const anchor = ReseedRoiAnchor(snap, 5, 150.0, 120.0);
	ASSERT_TRUE(anchor.valid);
	EXPECT_EQ(5, anchor.frame);
	EXPECT_DOUBLE_EQ(150.0, anchor.center_x);
	EXPECT_DOUBLE_EQ(120.0, anchor.center_y);
	EXPECT_DOUBLE_EQ(0.0, anchor.m00);
	EXPECT_DOUBLE_EQ(-1.0, anchor.m01);
	EXPECT_DOUBLE_EQ(1.0, anchor.m10);
	EXPECT_DOUBLE_EQ(0.0, anchor.m11);

	// The Continue seed rectangle is already storage at the seed frame. An
	// identity linear part would rotate it again; the reseed pose must not.
	RoiRect const roi{142, 108, 16, 24};
	RoiRect const mapped = MapRoiToFrame(snap, 5, roi, anchor);
	EXPECT_EQ(roi.x, mapped.x);
	EXPECT_EQ(roi.y, mapped.y);
	EXPECT_EQ(roi.w, mapped.w);
	EXPECT_EQ(roi.h, mapped.h);

	RoiRideAnchor identity{true, 5, 150.0, 120.0, 1.0, 0.0, 0.0, 1.0};
	RoiRect const doubled = MapRoiToFrame(snap, 5, roi, identity);
	EXPECT_NE(roi.x, doubled.x);
	EXPECT_NE(roi.y, doubled.y);
}

// Continue over an existing Similarity trajectory must not reset the
// published transform to the new template's frame: the reseeded backend
// measures against the reseed frame's current pose, so the session composes
// that accumulated base back in and samples stay origin-relative.
TEST(motion_track_session, similarity_continue_composes_reseed_base) {
	ScriptedSimilarityBackend backend;
	FlatReader reader;
	RoiRect const roi{.x = 88, .y = 72, .w = 24, .h = 16};

	MotionTrackSession session(Domains(0, 39), roi, TrackDirection::Forward,
							   TrackModel::Similarity);
	session.SetBackendSeed(0, roi, 100.0, 80.0);

	auto lease = FreshLease();
	auto handle = lease->Acquire();
	// First pass: +2 deg per frame. The progress hook fires after frame 13's
	// step but before its sample is pushed, and the cancel is only observed
	// at the next loop top, so Ok 0..13 end up committed.
	backend.step_rotation = 2.0 * kDeg;
	int done = 0;
	auto stop = session.RunAnalyze(
		reader, backend, handle,
		{.user_cancelled = [&] { return done >= 13; },
		 .progress = [&](int d, int) { done = d; }});
	EXPECT_EQ(AnalyzeStopReason::UserCanceled, stop);

	auto before = session.Capture();
	auto const *s12 = FindSample(before->samples, 12);
	ASSERT_NE(nullptr, s12);
	EXPECT_NEAR(24.0 * kDeg, SampleRotation(*s12), 1e-9);
	std::vector<TrackSample> const old_samples = before->samples;

	// Continue at frame 16, which has no sample: the base must come from the
	// nearest Ok sample (frame 13), i.e. R(26 deg) -- not identity.
	session.SetBackendSeed(16, roi, 132.0, 80.0);
	backend.step_rotation = 0.5 * kDeg;
	handle = lease->Acquire();
	stop = session.RunAnalyze(reader, backend, handle, {});
	EXPECT_EQ(AnalyzeStopReason::Completed, stop);

	auto snap = session.Capture();
	// Manual seed at 16: linear part equals the reseed base, translation
	// anchored at the origin center (frame 0's center = 100, 80).
	auto const *seed16 = FindSample(snap->samples, 16);
	ASSERT_NE(nullptr, seed16);
	EXPECT_EQ(TrackStatus::Ok, seed16->status);
	EXPECT_NEAR(26.0 * kDeg, SampleRotation(*seed16), 1e-9);
	EXPECT_NEAR(1.0, std::hypot(seed16->transform.matrix[0], seed16->transform.matrix[3]),
				1e-9);
	EXPECT_NEAR(32.0, seed16->transform.matrix[2], 1e-9);
	EXPECT_NEAR(0.0, seed16->transform.matrix[5], 1e-9);

	// Frames the second run never reached keep their first-pass samples
	// byte-for-byte; 14..15 were never tracked (cancelled first pass).
	for (auto const& s : old_samples) {
		auto const *b = FindSample(snap->samples, s.frame);
		ASSERT_NE(nullptr, b) << "frame " << s.frame;
		EXPECT_TRUE(s.transform.matrix == b->transform.matrix)
			<< "frame " << s.frame;
		EXPECT_DOUBLE_EQ(s.center_x, b->center_x) << "frame " << s.frame;
	}
	EXPECT_EQ(nullptr, FindSample(snap->samples, 14));

	// Reseeded frames: published rotation is base (26 deg) composed with the
	// template-relative increment (0.5 deg/frame from frame 16) -- angles add
	// under rotation composition, instead of restarting from identity. The
	// scripted centers drift as 100 + 2 * frame with the origin at (100, 80).
	for (int f = 17; f <= 39; ++f) {
		auto const *s = FindSample(snap->samples, f);
		ASSERT_NE(nullptr, s) << "frame " << f;
		EXPECT_EQ(TrackStatus::Ok, s->status) << "frame " << f;
		EXPECT_NEAR(26.0 * kDeg + (f - 16) * 0.5 * kDeg,
					SampleRotation(*s), 1e-9)
			<< "frame " << f;
		EXPECT_NEAR(1.0, std::hypot(s->transform.matrix[0], s->transform.matrix[3]),
					1e-9)
			<< "frame " << f;
		EXPECT_NEAR(100.0 + 2.0 * f, s->center_x, 1e-9) << "frame " << f;
	}
}

// Translation Continue regression: the reseed base is identity (translation
// samples carry no rotation/scale), so every published sample -- the manual
// seed included -- keeps the identity linear part and the trajectory stays
// translation-only.
TEST(motion_track_session, continue_keeps_translation_linear_identity) {
	auto scene = MakeScene(kVx, 0.0);
	SyntheticFrameReader reader(scene);
	TranslationTrackerBackend backend;

	MotionTrackSession session(Domains(0, 39), kObject,
							   TrackDirection::Forward,
							   TrackModel::Translation);
	session.SetBackendSeed(0, kObject, CenterX(0), CenterY());

	auto lease = FreshLease();
	auto handle = lease->Acquire();
	EXPECT_EQ(AnalyzeStopReason::Completed,
			  session.RunAnalyze(reader, backend, handle, {}));

	session.SetBackendSeed(25, RoiRect{.x = 40 + static_cast<int>(kVx * 25), .y = 30, .w = 24, .h = 16},
						   CenterX(25), CenterY());
	handle = lease->Acquire();
	EXPECT_EQ(AnalyzeStopReason::Completed,
			  session.RunAnalyze(reader, backend, handle, {}));

	auto snap = session.Capture();
	ASSERT_EQ(snap->samples.size(), size_t(40));
	for (auto const& s : snap->samples) {
		ASSERT_EQ(TrackStatus::Ok, s.status) << "frame " << s.frame;
		EXPECT_DOUBLE_EQ(1.0, s.transform.matrix[0]) << "frame " << s.frame;
		EXPECT_DOUBLE_EQ(0.0, s.transform.matrix[1]) << "frame " << s.frame;
		EXPECT_DOUBLE_EQ(0.0, s.transform.matrix[3]) << "frame " << s.frame;
		EXPECT_DOUBLE_EQ(1.0, s.transform.matrix[4]) << "frame " << s.frame;
	}
	// Translation keeps anchoring at the shared origin across the reseed.
	auto const *last = FindSample(snap->samples, 39);
	ASSERT_NE(nullptr, last);
	EXPECT_NEAR(last->center_x - snap->origin_center_x,
				last->transform.matrix[2], 1e-9);
}

// With no Ok sample to measure a base from (samples invalidated), Continue
// degrades to the old behavior: identity linear parts and a fresh origin at
// the new seed.
TEST(motion_track_session, continue_after_invalidate_uses_identity_base) {
	ScriptedSimilarityBackend backend;
	FlatReader reader;
	RoiRect const roi{.x = 88, .y = 72, .w = 24, .h = 16};

	MotionTrackSession session(Domains(0, 39), roi, TrackDirection::Forward,
							   TrackModel::Similarity);
	session.SetBackendSeed(0, roi, 100.0, 80.0);
	backend.step_rotation = 2.0 * kDeg;

	auto lease = FreshLease();
	auto handle = lease->Acquire();
	EXPECT_EQ(AnalyzeStopReason::Completed,
			  session.RunAnalyze(reader, backend, handle, {}));

	// Reseeding here would capture a non-identity base (frame 20 sits at
	// R(40 deg)); Invalidate must throw it away together with the samples.
	session.SetBackendSeed(20, roi, 140.0, 80.0);
	session.Invalidate(AnalyzeStopReason::Error, "test invalidation");

	handle = lease->Acquire();
	EXPECT_EQ(AnalyzeStopReason::Completed,
			  session.RunAnalyze(reader, backend, handle, {}));

	auto snap = session.Capture();
	EXPECT_EQ(20, snap->origin_seed_frame);
	auto const *seed = FindSample(snap->samples, 20);
	ASSERT_NE(nullptr, seed);
	EXPECT_EQ(TrackStatus::Ok, seed->status);
	EXPECT_DOUBLE_EQ(1.0, seed->transform.matrix[0]);
	EXPECT_DOUBLE_EQ(0.0, seed->transform.matrix[1]);
	EXPECT_DOUBLE_EQ(0.0, seed->transform.matrix[3]);
	EXPECT_DOUBLE_EQ(1.0, seed->transform.matrix[4]);
	EXPECT_DOUBLE_EQ(0.0, seed->transform.matrix[2]);
	for (int f = 21; f <= 39; ++f) {
		auto const *s = FindSample(snap->samples, f);
		ASSERT_NE(nullptr, s) << "frame " << f;
		EXPECT_NEAR((f - 20) * 2.0 * kDeg, SampleRotation(*s), 1e-9)
			<< "frame " << f;
	}
}
