#include <main.h>

#include "../../src/motion_track/session.h"
#include "../../src/motion_track/synthetic_frame_reader.h"
#include "../../src/motion_track/translation_backend.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace {
using namespace aegisub::motion_track;

RoiRect const kObject{40, 30, 24, 16};
double const kVx = 5.0;

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
		auto const* s = FindSample(snap->samples, f);
		ASSERT_NE(nullptr, s);
		EXPECT_NEAR(CenterX(f), s->center_x, 0.25) << "frame " << f;
	}
	// Origin-relative published transform.
	auto const* last = FindSample(snap->samples, 39);
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
	auto const* s10 = FindSample(snap->samples, 10);
	ASSERT_NE(nullptr, s10);
	if (auto const* s12 = FindSample(snap->samples, 12))
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

	auto const* seed_sample = FindSample(snap->samples, 11);
	ASSERT_NE(nullptr, seed_sample);
	EXPECT_EQ(TrackStatus::Ok, seed_sample->status);

	auto const* prev = FindSample(snap->samples, 10);
	auto const* next = FindSample(snap->samples, 12);
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
		auto const* s = FindSample(snap->samples, f);
		if (s) EXPECT_EQ(TrackStatus::Missing, s->status) << "frame " << f;
	}
	auto const* ok26 = FindSample(snap->samples, 24);
	EXPECT_NE(nullptr, ok26);
}

TEST(motion_track_session, range_caps_match_documented_thresholds) {
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

	lease_state->Begin(); // retire before the run starts
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
		auto const* s = FindSample(snap->samples, f);
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
	auto const* s10 = FindSample(snap->samples, 10);
	ASSERT_NE(nullptr, s10);
	EXPECT_EQ(TrackStatus::Ok, s10->status);
	auto const* s29 = FindSample(snap->samples, 29);
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
	auto const* seed = FindSample(snap->samples, 0);
	ASSERT_NE(nullptr, seed);
	EXPECT_EQ(TrackStatus::Ok, seed->status);
	auto const* last = FindSample(snap->samples, 20);
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
		auto const* s = FindSample(snap->samples, f);
		ASSERT_NE(nullptr, s) << "frame " << f;
		EXPECT_EQ(TrackStatus::Ok, s->status) << "frame " << f;
	}
}
