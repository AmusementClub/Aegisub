#pragma once

// All mutable working state is touched only
// on the calling (progress-task) thread; consumers read an atomically
// published immutable snapshot.

#include "apply_plan.h"
#include "../async_video_provider.h"
#include "frame_reader.h"
#include "synthetic_frame_reader.h"
#include "translation_backend.h"
#include "types.h"
#include "video_provider_lease.h"

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace aegisub::motion_track {

/// Immutable snapshot published after every committed batch / reset /
/// invalidation. Deep-copies everything; safe to hold across publishes.
struct MotionTrackSnapshot {
	std::uint64_t revision = 0;
	RoiRect roi;
	int origin_seed_frame = -1;
	double origin_center_x = 0.0, origin_center_y = 0.0;
	int backend_seed_frame = -1;
	FrameInterval decode_interval;
	FrameInterval direction_domain;
	TrackDirection direction = TrackDirection::Bidirectional;
	TrackModel model = TrackModel::Translation;
	std::vector<TrackSample> samples; // ascending by frame; use FindSample
	/// Fade intervals derived from the samples' visibility curve (both arms;
	/// reuses align_video_fade's plateau/curve fitting -- see session.cpp).
	/// Default-constructed (all-undetected) until a run completes and after
	/// Invalidate / re-seeding.
	FadeInterval fade_interval;
	int success_count = 0;
	int failure_count = 0;
	bool analyze_running = false;
	AnalyzeStopReason stop_reason = AnalyzeStopReason::None;
	std::uint64_t provider_generation = 0;
	std::string backend_name;
	std::string diagnostic_message;
	int storage_width = 0;
	int storage_height = 0;
};

struct SessionDomains {
	FrameInterval decode_interval;  // inclusive
	FrameInterval direction_domain; // inclusive
	int video_frame_count = 0;
	int storage_width = 0;
	int storage_height = 0;
	/// 0 = backend/doc default; tests may pin a smaller capture radius.
	int search_radius_override = 0;
};

/// The ride affine at one frame. The dialog ROI is expressed in the
/// coordinate space of the frame it was last edited at; the anchor is that
/// frame's tracked center (absolute storage) and linear part, and makes the
/// ROI's space concrete. Invalid when the frame has no Ok sample -- the ROI
/// is then plain storage coordinates and no riding mapping is defined.
struct RoiRideAnchor {
	bool valid = false;
	int frame = -1;
	double center_x = 0.0;
	double center_y = 0.0;
	/// Linear part of the sample's transform; identity for Translation.
	double m00 = 1.0, m01 = 0.0, m10 = 0.0, m11 = 1.0;
};

RoiRideAnchor RideAnchorAtFrame(MotionTrackSnapshot const& snap, int frame);

/// Anchor for a Continue/reseed: the new seed center in storage, with the
/// linear part of the Ok sample nearest `frame` (identity when there is
/// none, or for Translation). The manual seed sample carries that linear
/// part, so the overlay/spin ROI — already in storage at the seed frame —
/// must be pinned to it rather than to identity.
RoiRideAnchor ReseedRoiAnchor(MotionTrackSnapshot const& snap, int frame,
							  double center_x, double center_y);

/// Maps an ROI expressed at `anchor`'s frame to storage coordinates at
/// `frame`, riding the trajectory between the two: the bounding box of the
/// mapped corners (exact for Translation, inflating slightly for rotated
/// Similarity quads). Returns `roi` unchanged when either end lacks an Ok
/// sample, so callers fall back to the raw rectangle.
RoiRect MapRoiToFrame(MotionTrackSnapshot const& snap, int frame, RoiRect roi,
					  RoiRideAnchor const& anchor);

/// Runs one frame fetch inside a provider-lifetime-checked batch. Receives a
/// closure that performs the actual FetchGray against a caller-supplied
/// MotionFrameReader; production implementations bind it to a
/// RawBatchMotionFrameReader over RawFrameAccess, tests bind it to any
/// reader directly.
using RawBatchExecutor =
	std::function<RawVideoBatchStatus(
		std::function<FrameReadResult(MotionFrameReader&)> const&)>;

struct AnalyzeRunHooks {
	std::function<bool()> user_cancelled;              // polled between frames
	std::function<void(int done, int total)> progress; // optional
	/// Production path: runs one worker batch per frame. When unset (core
	/// tests), `reader` is used directly on the calling thread. The seed
	/// fetch goes through the same path — a null `reader` with run_batch set
	/// must still work end to end.
	RawBatchExecutor run_batch;
};

/// Result of the pre-flight range checks.
enum class RangeCheck {
	Ok,
	NeedsConfirmation, // decode span > 1500 frames
	TooLong,           // decode span > 10000 frames -> refuse
};

/// Range cap used by the session and the dialog so Analyze can refuse a
/// TooLong span (or ask before a long run) without constructing a session
/// or touching an existing one.
RangeCheck CheckDecodeInterval(FrameInterval interval);

class MotionTrackSession {
	public:
	MotionTrackSession(SessionDomains domains, RoiRect roi,
					   TrackDirection direction, TrackModel model,
					   TranslationTrackerConfig backend_config = {});

	RangeCheck CheckRange() const;

	/// Origin seed: first successful Analyze fixes it; Continue never moves
	/// it. Backend seed: where Reset happens; Continue moves this.
	/// Continue flow: the caller moves/redraws the ROI onto the object at
	///  (mirroring the user dragging the box), then seeds there.
	void SetBackendSeed(int frame, RoiRect roi, double center_x,
						double center_y);
	int BackendSeedFrame() const { return backend_seed_frame_; }

	/// Runs the analyze protocol on the current thread (the progress-task
	/// thread). Commits per-batch deltas transactionally and republishes the
	/// snapshot after each commit. Returns the terminal stop reason.
	///
	/// Stop-reason aggregation across arms (Bidirectional runs two):
	///  - hard stops (user cancel, provider changed, decode error, backend
	///    error) abort the remaining arm;
	///  - soft stops (Completed, ConsecutiveFailures) only end their own arm;
	///  - a hard stop wins over ConsecutiveFailures, which wins over plain
	///    Completed.
	/// Fetch failures keep the committed prefix (matching cancellation);
	/// only a provider change invalidates the whole trajectory.
	///
	/// Consumes `lease_handle`: it is released before returning, on every
	/// path, so a provider retirement waiting in WaitForLeaseDrain is not
	/// held up by a finished run. Continue re-acquires a fresh handle.
	AnalyzeStopReason RunAnalyze(MotionFrameReader& reader,
								 TrackerBackend& backend,
								 VideoProviderLeaseState::Handle& lease_handle,
								 AnalyzeRunHooks hooks);

	/// Clears all samples and re-derives origin from the next successful run.
	void Invalidate(AnalyzeStopReason reason, std::string message);

	AnalyzeStopReason LastStopReason() const { return last_stop_; }

	std::shared_ptr<const MotionTrackSnapshot> Capture() const {
		return published_.load();
	}

	private:
	struct DirectionState {
		bool has_center = false;
		double last_x = 0.0, last_y = 0.0;
		double v_pred_x = 0.0, v_pred_y = 0.0; // EMA of accepted velocities
		double prev_x = 0.0, prev_y = 0.0;
		bool has_prev = false;
		/// Seed-relative pose carried between steps (similarity models use
		/// it to initialize the per-frame refinement); reset on re-seed.
		double last_rotation = 0.0;
		double last_scale = 1.0;
		int consecutive_failures = 0;
	};

	SessionDomains domains_;
	RoiRect roi_;
	TrackDirection direction_;
	TrackModel model_;
	TranslationTrackerConfig backend_config_;

	// Working state (analyze-thread confined).
	int origin_seed_frame_ = -1;
	double origin_center_x_ = 0.0, origin_center_y_ = 0.0;
	bool has_origin_ = false;
	int backend_seed_frame_ = -1;
	double backend_seed_x_ = 0.0, backend_seed_y_ = 0.0;
	/// Linear part of the published transform at the frame the backend
	/// template was last rebuilt on, captured by SetBackendSeed from the Ok
	/// sample nearest the new seed ({m00, m01, m10, m11}, column-vector
	/// layout). A re-seeded backend measures every step against its new
	/// template -- the reseed frame's current pose -- so published samples
	/// must compose this base back in (template-relative step · base) to stay
	/// origin-relative; identity on a fresh session, after Invalidate, and
	/// for the translation model (whose steps are identity anyway).
	struct ReseedBase {
		double m00 = 1.0, m01 = 0.0, m10 = 0.0, m11 = 1.0;
	};
	ReseedBase reseed_base_;
	DirectionState fwd_, back_;
	std::vector<TrackSample> samples_;
	/// Fade intervals extracted from samples_ at the end of the last
	/// completed run (ExtractFadeInterval); cleared on Invalidate and
	/// re-seeding because the curve around a new seed is not measured yet.
	FadeInterval fade_interval_;

	std::uint64_t revision_ = 0;
	std::atomic<std::shared_ptr<const MotionTrackSnapshot>> published_;
	AnalyzeStopReason last_stop_ = AnalyzeStopReason::None;
	std::string diagnostic_;
	std::string backend_name_; ///< from TrackerBackend::Name(), set per run
	/// Lease generation of the run that produced the current samples, taken
	/// from the handle RunAnalyze was given. 0 until the first run.
	std::uint64_t provider_generation_ = 0;

	void WriteSeedSample();
	void CommitSamples(std::vector<TrackSample> batch);
	void Publish(bool running);
	/// Derives the fade-in/-out intervals from the committed visibility
	/// curve around the backend seed (the fully-visible anchor). Reuses
	/// align_video_fade's plateau/curve fitting with the parameter
	/// conventions dialog_align's key-point scan uses (plateau and
	/// confirmation sample counts, low-signal early stop, capped fit
	/// window); see the constants in session.cpp.
	static FadeInterval ExtractFadeInterval(
		std::vector<TrackSample> const& samples, int seed_frame);
	static double EffectiveRadius(int roi_max_side);
	int SearchRadiusForRoi() const;
};

/// Copies the published trajectory fields a planner run needs. Does not
/// touch script resolution, timecodes, seed time, or apply options — those
/// come from the file/video/UI. Always copies `fade_interval` so the
/// dialog's "Write \\fad from fade" checkbox has data to apply.
void FillApplyInputFromSnapshot(ApplyPlanInput& input,
								MotionTrackSnapshot const& snap);

} // namespace aegisub::motion_track
