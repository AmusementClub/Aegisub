#include "session.h"

#include "../align_video_fade.h"

#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <cmath>

namespace aegisub::motion_track {

namespace {
constexpr int kConfirmFrames = 1500;
constexpr int kRejectFrames = 10000;
constexpr int kMaxConsecutiveFailures = 3;

// --- Fade interval extraction, phase B ------------------------------------
// Parameter conventions mirror dialog_align's key-point fade scan
// (FindKeyPointRange in async_video_provider.cpp), so the same fitting
// machinery sees the same shapes it was tuned on:
//   - plateau / confirmation sample count 4 (plateau_samples /
//     plateau_confirmation there);
//   - low-signal early stop after 4 consecutive samples at or below 8% of
//     the plateau level (low_signal_confirmation and the 0.08 factor);
//   - the outward fit window is capped at 240 samples (dialog_align's
//     max_fade_frames absolute clamp; its 2000 ms derivation needs
//     timecodes, which the frame-domain session does not have).
constexpr int kFadePlateauSamples = 4;
constexpr int kFadeLowSignalConfirmation = 4;
constexpr double kFadeLowSignalFraction = 0.08;
constexpr int kFadeMaxFitFrames = 240;
constexpr int kFadeMinOutsideSamples = 3;

AnalyzeStopReason StopReasonFromFrameRead(FrameReadStatus status) {
	switch (status) {
		case FrameReadStatus::Ok:
			return AnalyzeStopReason::None;
		case FrameReadStatus::ProviderChanged:
			return AnalyzeStopReason::ProviderChanged;
		case FrameReadStatus::FrameUnavailable:
			return AnalyzeStopReason::FrameUnavailable;
		case FrameReadStatus::DecodeError:
			return AnalyzeStopReason::DecodeError;
		default:
			return AnalyzeStopReason::Error;
	}
}

TrackSample const *NearestOkSample(std::vector<TrackSample> const& samples,
								   int frame) {
	TrackSample const *base = nullptr;
	for (auto const& s : samples) {
		if (s.status != TrackStatus::Ok)
			continue;
		if (!base || std::abs(s.frame - frame) < std::abs(base->frame - frame))
			base = &s;
	}
	return base;
}
} // namespace

RangeCheck CheckDecodeInterval(FrameInterval interval) {
	int const span = interval.last - interval.first + 1;
	if (span > kRejectFrames)
		return RangeCheck::TooLong;
	if (span > kConfirmFrames)
		return RangeCheck::NeedsConfirmation;
	return RangeCheck::Ok;
}

MotionTrackSession::MotionTrackSession(SessionDomains domains, RoiRect roi,
									   TrackDirection direction,
									   TrackModel model,
									   TranslationTrackerConfig backend_config)
	: domains_(domains), roi_(roi), direction_(direction), model_(model),
	  backend_config_(std::move(backend_config)) {
	Publish(false);
}

RangeCheck MotionTrackSession::CheckRange() const {
	return CheckDecodeInterval(domains_.decode_interval);
}

void MotionTrackSession::SetBackendSeed(int frame, RoiRect roi,
										double center_x, double center_y) {
	roi_ = roi;
	backend_seed_frame_ = frame;
	backend_seed_x_ = center_x;
	backend_seed_y_ = center_y;
	// The next run rebuilds the backend template from the reseed frame's
	// pixels in their current pose, so the backend reports steps relative to
	// that pose -- not relative to the original seed. Remember the old
	// trajectory's accumulated linear part at the Ok sample nearest the new
	// seed (identity when there is none) so WriteSeedSample and RunAnalyze
	// can compose it back in and published samples stay origin-relative.
	// Captured before the seed-frame sample is erased below.
	reseed_base_ = {};
	if (TrackSample const *base = NearestOkSample(samples_, frame)) {
		reseed_base_.m00 = base->transform.matrix[0];
		reseed_base_.m01 = base->transform.matrix[1];
		reseed_base_.m10 = base->transform.matrix[3];
		reseed_base_.m11 = base->transform.matrix[4];
	}
	// Continue replaces whatever sample sat on the seed frame with the manual
	// Ok seed written at the start of the next run.
	samples_.erase(
		std::remove_if(samples_.begin(), samples_.end(),
					   [frame](TrackSample const& s) { return s.frame == frame; }),
		samples_.end());
	fwd_ = DirectionState{};
	back_ = DirectionState{};
	fwd_.has_center = true;
	fwd_.last_x = center_x;
	fwd_.last_y = center_y;
	back_ = fwd_;
	// The visibility curve around the new seed is not measured yet; the
	// next run's completion repopulates it.
	fade_interval_ = FadeInterval{};
	Publish(false);
}

void MotionTrackSession::Invalidate(AnalyzeStopReason reason,
									std::string message) {
	samples_.clear();
	// With the samples gone there is no trajectory left to measure a reseed
	// base against; the next run re-derives everything from a fresh seed.
	reseed_base_ = {};
	has_origin_ = false;
	fade_interval_ = FadeInterval{};
	last_stop_ = reason;
	diagnostic_ = std::move(message);
	Publish(false);
}

void MotionTrackSession::WriteSeedSample() {
	TrackSample seed;
	seed.frame = backend_seed_frame_;
	seed.model = model_;
	seed.status = TrackStatus::Ok;
	seed.confidence = 1.0;
	seed.center_x = backend_seed_x_;
	seed.center_y = backend_seed_y_;
	// The seed is its own fully-visible reference for the fade slice.
	seed.fade_visibility = 1.0;
	if (!has_origin_) {
		has_origin_ = true;
		origin_seed_frame_ = backend_seed_frame_;
		origin_center_x_ = backend_seed_x_;
		origin_center_y_ = backend_seed_y_;
	}
	// A re-seeded backend measures against the template captured at the new
	// seed's current pose, so the manual seed carries the old trajectory's
	// accumulated linear part (identity on a fresh session); the translation
	// entries stay the origin-center delta, shared by both models.
	seed.transform.matrix[0] = reseed_base_.m00;
	seed.transform.matrix[1] = reseed_base_.m01;
	seed.transform.matrix[3] = reseed_base_.m10;
	seed.transform.matrix[4] = reseed_base_.m11;
	seed.transform.matrix[2] = backend_seed_x_ - origin_center_x_;
	seed.transform.matrix[5] = backend_seed_y_ - origin_center_y_;
	auto it = std::lower_bound(
		samples_.begin(), samples_.end(), seed.frame,
		[](TrackSample const& s, int f) { return s.frame < f; });
	if (it != samples_.end() && it->frame == seed.frame)
		*it = seed;
	else
		samples_.insert(it, seed);
}

void MotionTrackSession::CommitSamples(std::vector<TrackSample> batch) {
	// Replace any existing samples at the batch's frames: Continue can
	// re-track frames an earlier run already committed, and the fresh
	// estimate must win — duplicates would leave FindSample returning the
	// stale entry.
	for (auto const& s : batch) {
		auto it = std::lower_bound(
			samples_.begin(), samples_.end(), s.frame,
			[](TrackSample const& e, int f) { return e.frame < f; });
		if (it != samples_.end() && it->frame == s.frame)
			samples_.erase(it);
	}
	for (auto& s : batch)
		samples_.push_back(std::move(s));
	std::sort(samples_.begin(), samples_.end(),
			  [](TrackSample const& a, TrackSample const& b) {
				  return a.frame < b.frame;
			  });
}

void MotionTrackSession::Publish(bool running) {
	auto snapshot = std::make_shared<MotionTrackSnapshot>();
	snapshot->revision = ++revision_;
	snapshot->roi = roi_;
	snapshot->origin_seed_frame = origin_seed_frame_;
	snapshot->origin_center_x = origin_center_x_;
	snapshot->origin_center_y = origin_center_y_;
	snapshot->backend_seed_frame = backend_seed_frame_;
	snapshot->decode_interval = domains_.decode_interval;
	snapshot->direction_domain = domains_.direction_domain;
	snapshot->direction = direction_;
	snapshot->model = model_;
	snapshot->samples = samples_;
	snapshot->fade_interval = fade_interval_;
	snapshot->analyze_running = running;
	snapshot->stop_reason = last_stop_;
	snapshot->diagnostic_message = diagnostic_;
	snapshot->storage_width = domains_.storage_width;
	snapshot->storage_height = domains_.storage_height;
	for (auto const& s : snapshot->samples) {
		if (s.status == TrackStatus::Ok)
			++snapshot->success_count;
		else if (s.status == TrackStatus::Failed)
			++snapshot->failure_count;
	}
	if (backend_name_.empty())
		backend_name_ = "ncc-pyramid";
	snapshot->backend_name = backend_name_;
	snapshot->provider_generation = provider_generation_;
	published_.store(std::move(snapshot));
}

AnalyzeStopReason MotionTrackSession::RunAnalyze(
	MotionFrameReader& reader, TrackerBackend& backend,
	VideoProviderLeaseState::Handle& lease_handle, AnalyzeRunHooks hooks) {
	// The run owns the lease: whoever passed it in gets it back released, on
	// every exit path. Doing this only at the bottom leaked the lease through
	// the range-check and seed-transaction early returns, and WaitForLeaseDrain
	// would then block until the dialog closed.
	auto release_lease =
		agi::make_scope_exit([&] { lease_handle.Release(); });
	provider_generation_ = lease_handle.Generation();

	if (CheckRange() == RangeCheck::TooLong) {
		Invalidate(AnalyzeStopReason::Error, "range exceeds the hard cap");
		return AnalyzeStopReason::Error;
	}

	backend_name_ = std::string(backend.Name());
	auto cancelled = [&]() -> bool {
		return hooks.user_cancelled && hooks.user_cancelled();
	};

	// One-frame fetch through either the production batch executor or the
	// plain reader (core tests). Shared by the seed transaction and the
	// stepping loop so both always traverse the same provider path.
	//
	// One batch per frame means one worker Sync per frame. That is not
	// batchable as the loop stands: the crop for frame N+1 comes out of
	// backend.Step on frame N, so the fetch list is not known ahead of time.
	// Amortizing the round trip would mean running the stepping loop itself
	// inside the batch callback, i.e. on the video worker thread, with
	// progress/cancel/Publish crossing threads -- a redesign, not a tweak.
	auto fetch_frame = [&](int frame, RoiRect crop, GrayPatch& out)
		-> FrameReadResult {
		if (!hooks.run_batch)
			return reader.FetchGray(frame, crop, out);
		switch (hooks.run_batch([&](MotionFrameReader& batched) {
			return batched.FetchGray(frame, crop, out);
		})) {
			case RawVideoBatchStatus::Completed:
				return FrameReadResult{FrameReadStatus::Ok, {}};
			case RawVideoBatchStatus::ProviderChanged:
				return FrameReadResult{FrameReadStatus::ProviderChanged,
									   "provider changed"};
			case RawVideoBatchStatus::FrameUnavailable:
				return FrameReadResult{FrameReadStatus::FrameUnavailable,
									   "frame unavailable"};
			case RawVideoBatchStatus::DecodeError:
				return FrameReadResult{FrameReadStatus::DecodeError,
									   "decode failed"};
			default:
				return FrameReadResult{FrameReadStatus::Error,
									   "batch failed"};
		}
	};

	// Seed fetch + Reset as an independent single-frame transaction.
	{
		GrayPatch patch;
		auto read = fetch_frame(backend_seed_frame_, roi_, patch);
		if (read.status != FrameReadStatus::Ok) {
			auto const reason = StopReasonFromFrameRead(read.status);
			Invalidate(reason, read.message);
			return reason;
		}
		if (backend.Reset(TrackerSeed{
				model_, roi_, patch.View(),
				backend_seed_frame_}) != TrackStatus::Ok) {
			Invalidate(AnalyzeStopReason::Error, "backend reset failed");
			return AnalyzeStopReason::Error;
		}
		WriteSeedSample();
		Publish(false);
	}

	struct Arm {
		bool active = false;
		int step;  // +1 forward, -1 backward
		int first; // first frame to step (exclusive of seed)
		int last;  // last frame inclusive
		DirectionState *state;
	};
	std::vector<Arm> arms;
	if (direction_ != TrackDirection::Backward)
		arms.push_back({true, +1, backend_seed_frame_ + 1,
						domains_.direction_domain.last, &fwd_});
	if (direction_ != TrackDirection::Forward)
		arms.push_back({true, -1, backend_seed_frame_ - 1,
						domains_.direction_domain.first, &back_});

	last_stop_ = AnalyzeStopReason::None;
	Publish(true);

	// The backward arm counts down (first > last), so use the absolute span:
	// a signed sum would subtract the backward frames from the total and can
	// even go negative, which makes SetProgress report nonsense.
	int total_frames = 0;
	for (auto const& arm : arms)
		total_frames += std::abs(arm.last - arm.first) + 1;
	int done_frames = 0;

	// Hard stops abort the remaining arm; soft stops (Completed,
	// ConsecutiveFailures) only end the arm that produced them.
	AnalyzeStopReason hard_stop = AnalyzeStopReason::None;
	bool any_consecutive_failures = false;

	for (auto& arm : arms) {
		if (hard_stop != AnalyzeStopReason::None)
			break;

		// The two arms share one backend instance; its per-arm tracking
		// state (last match, held window, fade state machine) belongs to
		// the frames on this arm's side of the seed only, or the second
		// arm's first steps would hold at the first arm's last position.
		backend.BeginDirectionArm();

		std::vector<TrackSample> delta;
		auto commit = [&] {
			if (!delta.empty())
				CommitSamples(delta);
			delta.clear();
			Publish(true);
		};

		int cursor_frame = arm.first;
		int frames_since_commit = 0;
		while (true) {
			if (cancelled()) {
				commit();
				hard_stop = AnalyzeStopReason::UserCanceled;
				break;
			}
			if (lease_handle.ChangeRequested()) {
				// Provider retirement: drop everything from this run.
				Invalidate(AnalyzeStopReason::ProviderChanged,
						   "video provider changed during analysis");
				hard_stop = AnalyzeStopReason::ProviderChanged;
				break;
			}
			bool const out_of_range =
				arm.step > 0 ? cursor_frame > arm.last : cursor_frame < arm.last;
			if (out_of_range) {
				commit();
				break;
			}

			// Velocity prior biases the search center; capture radius grows.
			double sx = arm.state->last_x + std::clamp(arm.state->v_pred_x, -24.0, 24.0);
			double sy = arm.state->last_y + std::clamp(arm.state->v_pred_y, -24.0, 24.0);

			auto const expected_left_x = static_cast<int>(std::floor(
				sx - (roi_.w - 1) / 2.0 + 0.5));
			auto const expected_left_y = static_cast<int>(std::floor(
				sy - (roi_.h - 1) / 2.0 + 0.5));
			int radius = SearchRadiusForRoi();
			if (domains_.search_radius_override <= 0) {
				// Widen the capture window with the unclamped predicted speed:
				// fast pans overshoot the ±24 px prior clamp, and the target
				// then leaves a base-radius crop before NCC ever sees it. The
				// extra margin covers prediction lag, not just displacement.
				double const speed = std::max(std::abs(arm.state->v_pred_x),
											  std::abs(arm.state->v_pred_y));
				radius = std::clamp(
					std::max(radius, int(std::ceil(speed)) + 32), radius, 256);
			}
			RoiRect crop{expected_left_x - radius, expected_left_y - radius,
						 roi_.w + 2 * radius, roi_.h + 2 * radius};

			GrayPatch patch;
			auto read = fetch_frame(cursor_frame, crop, patch);
			if (read.status == FrameReadStatus::ProviderChanged) {
				Invalidate(AnalyzeStopReason::ProviderChanged,
						   "video provider changed during analysis");
				hard_stop = AnalyzeStopReason::ProviderChanged;
				break;
			}
			if (read.status != FrameReadStatus::Ok) {
				// Keep the committed prefix (same as cancellation); only the
				// stop reason and diagnostic differ.
				commit();
				diagnostic_ = read.message.empty() ? "frame fetch failed"
												   : read.message;
				hard_stop = StopReasonFromFrameRead(read.status);
				break;
			}

			TrackStepRequest request;
			request.frame = cursor_frame;
			request.image = patch.View();
			request.image_origin_x = crop.x;
			request.image_origin_y = crop.y;
			request.search_center_x = sx;
			request.search_center_y = sy;
			request.init_rotation = arm.state->last_rotation;
			request.init_scale = arm.state->last_scale;
			auto step = backend.Step(request);

			++done_frames;
			if (hooks.progress)
				hooks.progress(done_frames, total_frames);

			if (step.status == TrackStatus::Ok) {
				// Fade-held steps re-report the previous center verbatim, so
				// vx/vy are exactly zero and the velocity prior decays
				// geometrically instead of pretending motion; a fade can no
				// longer kill the arm because these are normal Ok steps.
				double const vx = step.candidate_center_x - arm.state->last_x;
				double const vy = step.candidate_center_y - arm.state->last_y;
				arm.state->v_pred_x = 0.5 * arm.state->v_pred_x + 0.5 * vx;
				arm.state->v_pred_y = 0.5 * arm.state->v_pred_y + 0.5 * vy;
				arm.state->prev_x = arm.state->last_x;
				arm.state->prev_y = arm.state->last_y;
				arm.state->has_prev = true;
				arm.state->last_x = step.candidate_center_x;
				arm.state->last_y = step.candidate_center_y;
				arm.state->consecutive_failures = 0;
				arm.state->last_rotation = std::atan2(
					step.transform.matrix[3], step.transform.matrix[0]);
				arm.state->last_scale = std::hypot(
					step.transform.matrix[0], step.transform.matrix[3]);

				TrackSample sample;
				sample.frame = cursor_frame;
				sample.model = model_;
				sample.status = TrackStatus::Ok;
				sample.confidence = step.confidence;
				sample.residual = step.residual;
				sample.fade_visibility = step.fade_visibility;
				sample.center_x = step.candidate_center_x;
				sample.center_y = step.candidate_center_y;
				// The backend reports the linear part relative to its current
				// template. On a Continue that template is the reseed frame's
				// pose -- the accumulated linear part SetBackendSeed captured
				// -- so composing template-relative · base (column-vector
				// order: base applies first, the same structure as
				// MapRoiToFrame's dst·anchor⁻¹) keeps the published sample
				// origin-relative; on a fresh seed, after Invalidate, and for
				// the translation model both factors are identity and this is
				// a no-op. The translation entries are anchored at the origin
				// center, shared by both models. last_rotation/last_scale
				// above stay template-relative on purpose: they prime the
				// backend's per-step refinement against the current template.
				sample.transform = step.transform;
				sample.transform.matrix[0] =
					step.transform.matrix[0] * reseed_base_.m00 + step.transform.matrix[1] * reseed_base_.m10;
				sample.transform.matrix[1] =
					step.transform.matrix[0] * reseed_base_.m01 + step.transform.matrix[1] * reseed_base_.m11;
				sample.transform.matrix[3] =
					step.transform.matrix[3] * reseed_base_.m00 + step.transform.matrix[4] * reseed_base_.m10;
				sample.transform.matrix[4] =
					step.transform.matrix[3] * reseed_base_.m01 + step.transform.matrix[4] * reseed_base_.m11;
				sample.transform.matrix[2] =
					step.candidate_center_x - origin_center_x_;
				sample.transform.matrix[5] =
					step.candidate_center_y - origin_center_y_;
				delta.push_back(sample);
				cursor_frame += arm.step;
				if (++frames_since_commit >= 8) {
					frames_since_commit = 0;
					commit();
				}
				continue;
			}

			if (step.status == TrackStatus::Failed) {
				TrackSample sample;
				sample.frame = cursor_frame;
				sample.status = TrackStatus::Failed;
				sample.failure = step.failure;
				// Last accepted position: where the tracker lost the object;
				// the overlay draws failure marks there.
				sample.center_x = arm.state->last_x;
				sample.center_y = arm.state->last_y;
				delta.push_back(sample);
				commit();
				frames_since_commit = 0;
				++arm.state->consecutive_failures;
				cursor_frame += arm.step;
				if (arm.state->consecutive_failures >= kMaxConsecutiveFailures) {
					any_consecutive_failures = true;
					break;
				}
				continue;
			}

			// InvalidInput etc.: hard stop for this direction.
			commit();
			diagnostic_ = "backend rejected the step";
			hard_stop = AnalyzeStopReason::Error;
			break;
		}
	}

	if (hard_stop != AnalyzeStopReason::None)
		last_stop_ = hard_stop;
	else if (any_consecutive_failures)
		last_stop_ = AnalyzeStopReason::ConsecutiveFailures;
	else
		last_stop_ = AnalyzeStopReason::Completed;

	// Phase B: derive the fade intervals from the curve this run recorded
	// (the committed prefix is kept on soft and hard stops alike, so a
	// partially tracked ramp still reports what was observed).
	fade_interval_ = ExtractFadeInterval(samples_, backend_seed_frame_);

	// release_lease drops the lease once the final snapshot is out.
	Publish(false);
	return last_stop_;
}

void FillApplyInputFromSnapshot(ApplyPlanInput& input,
								MotionTrackSnapshot const& snap) {
	input.samples = snap.samples;
	input.model = snap.model;
	input.origin_center_x = snap.origin_center_x;
	input.origin_center_y = snap.origin_center_y;
	input.decode_interval = snap.decode_interval;
	input.direction_domain = snap.direction_domain;
	input.storage_width = snap.storage_width;
	input.storage_height = snap.storage_height;
	input.fade_interval = snap.fade_interval;
}

namespace {
struct Linear2 {
	double m00, m01, m10, m11;
};

Linear2 LinearFromSample(TrackSample const& s, TrackModel model) {
	if (model != TrackModel::Similarity)
		return {1.0, 0.0, 0.0, 1.0};
	return {s.transform.matrix[0], s.transform.matrix[1],
			s.transform.matrix[3], s.transform.matrix[4]};
}

bool Invert(Linear2 const& m, Linear2& out) {
	double const det = m.m00 * m.m11 - m.m01 * m.m10;
	if (std::abs(det) < 1e-9)
		return false;
	out = {m.m11 / det, -m.m01 / det, -m.m10 / det, m.m00 / det};
	return true;
}

Linear2 Mul(Linear2 const& a, Linear2 const& b) {
	return {a.m00 * b.m00 + a.m01 * b.m10,
			a.m00 * b.m01 + a.m01 * b.m11,
			a.m10 * b.m00 + a.m11 * b.m10,
			a.m10 * b.m01 + a.m11 * b.m11};
}
} // namespace

RoiRideAnchor RideAnchorAtFrame(MotionTrackSnapshot const& snap, int frame) {
	RoiRideAnchor anchor;
	auto const *s = FindSample(snap.samples, frame);
	if (!s || s->status != TrackStatus::Ok)
		return anchor;
	anchor.valid = true;
	anchor.frame = frame;
	anchor.center_x = s->center_x;
	anchor.center_y = s->center_y;
	Linear2 const m = LinearFromSample(*s, snap.model);
	anchor.m00 = m.m00;
	anchor.m01 = m.m01;
	anchor.m10 = m.m10;
	anchor.m11 = m.m11;
	return anchor;
}

RoiRect MapRoiToFrame(MotionTrackSnapshot const& snap, int frame, RoiRect roi,
					  RoiRideAnchor const& anchor) {
	if (!anchor.valid)
		return roi;
	auto const *dst = FindSample(snap.samples, frame);
	if (!dst || dst->status != TrackStatus::Ok)
		return roi;

	// Relative affine anchor -> destination: the ROI keeps its position
	// relative to the tracked object across the ride, so user edits made at
	// the anchor frame land where the box is displayed at the seed frame.
	Linear2 const dst_m = LinearFromSample(*dst, snap.model);
	Linear2 inv_anchor;
	if (!Invert(Linear2{anchor.m00, anchor.m01, anchor.m10, anchor.m11},
				inv_anchor))
		return roi;
	Linear2 const rel = Mul(dst_m, inv_anchor);
	auto const map = [&](double x, double y) {
		double const dx = x - anchor.center_x;
		double const dy = y - anchor.center_y;
		return std::pair<double, double>(
			dst->center_x + rel.m00 * dx + rel.m01 * dy,
			dst->center_y + rel.m10 * dx + rel.m11 * dy);
	};

	// Half-open corners, matching the grips the overlay draws (roi.x .. x+w).
	std::pair<double, double> const corners[4] = {
		map(roi.x, roi.y), map(roi.x + roi.w, roi.y), map(roi.x, roi.y + roi.h),
		map(roi.x + roi.w, roi.y + roi.h)};
	double min_x = corners[0].first, max_x = min_x;
	double min_y = corners[0].second, max_y = min_y;
	for (auto const& c : corners) {
		min_x = std::min(min_x, c.first);
		max_x = std::max(max_x, c.first);
		min_y = std::min(min_y, c.second);
		max_y = std::max(max_y, c.second);
	}
	// Inclusive enclosure of the mapped corners: rounding both extrema to
	// nearest can shrink a rotated/scaled quad and drop source pixels.
	int const x0 = int(std::floor(min_x));
	int const y0 = int(std::floor(min_y));
	int const x1 = int(std::ceil(max_x));
	int const y1 = int(std::ceil(max_y));
	return RoiRect{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

RoiRideAnchor ReseedRoiAnchor(MotionTrackSnapshot const& snap, int frame,
							  double center_x, double center_y) {
	RoiRideAnchor anchor;
	anchor.valid = true;
	anchor.frame = frame;
	anchor.center_x = center_x;
	anchor.center_y = center_y;
	if (TrackSample const *base = NearestOkSample(snap.samples, frame)) {
		Linear2 const m = LinearFromSample(*base, snap.model);
		anchor.m00 = m.m00;
		anchor.m01 = m.m01;
		anchor.m10 = m.m10;
		anchor.m11 = m.m11;
	}
	return anchor;
}

double MotionTrackSession::EffectiveRadius(int roi_max_side) {
	return std::min(256.0, std::max(48.0, double(roi_max_side)));
}

// Derives one direction's fade interval from the visibility curve,
// mirroring dialog_align's inside/outside walks around its strict boundary:
//   1. the seed-adjacent samples form the fully-visible platform
//      (BuildPlateauReference) that defines the plateau level and its band;
//   2. the plateau run is confirmed window-by-window
//      (FindConfirmedPlateauStart); the first failing window localizes the
//      ramp start (its first below-band sample), and the window just inside
//      is the confirmed plateau tail FitVisibilityCurve's contract requires;
//   3. the outside walk collects from the ramp start outward until the
//      low-signal early stop or the fit cap (dialog_align's max_fade_frames
//      clamp; the 2000 ms half of its derivation needs timecodes the
//      frame-domain session does not have);
//   4. reversed (deep end first) and concatenated with the plateau tail
//      walked toward the seed, the samples are ordered exactly as
//      FitVisibilityCurve demands: outside of the fade toward the
//      fully-visible anchor, ending in the confirmed plateau.
// The fit's inner_index then lands on the plateau tail's ramp-side edge --
// the last (fade-out) / first (fade-in) fully-visible frame -- and
// outer_index on the first persistently visible sample of the ramp.
FadeInterval MotionTrackSession::ExtractFadeInterval(
	std::vector<TrackSample> const& samples, int seed_frame) {
	FadeInterval interval;
	auto const *seed =
		seed_frame >= 0 ? FindSample(samples, seed_frame) : nullptr;
	if (!seed || seed->status != TrackStatus::Ok || seed->fade_visibility < 0.0)
		return interval;

	auto extract_direction = [&](int step) {
		FadeIntervalEdge edge;
		// Contiguous measured Ok samples from the seed outward; index 0 is
		// the seed itself (visibility 1.0 -- the template is its own
		// reference). A missing, failed or unmeasured frame ends the run:
		// the confirmation windows need contiguous samples, and the curve
		// beyond a gap was never observed.
		std::vector<double> vis{seed->fade_visibility};
		std::vector<int> frames{seed_frame};
		for (int f = seed_frame + step;; f += step) {
			auto const *s = FindSample(samples, f);
			if (!s || s->status != TrackStatus::Ok || s->fade_visibility < 0.0)
				break;
			vis.push_back(s->fade_visibility);
			frames.push_back(f);
		}
		int const n = int(vis.size());
		if (n < 2 * kFadePlateauSamples + kFadeMinOutsideSamples)
			return edge;

		auto const platform = align_video_fade::BuildPlateauReference(
			std::span<double const>(vis).first(kFadePlateauSamples));
		if (!platform.valid)
			return edge;

		// Confirmed plateau run from the seed: advance while the window at
		// each start still matches the platform reference. Stopping one
		// sample past the last confirming start leaves the failing window,
		// which contains the departure.
		int b = 0;
		while (b + kFadePlateauSamples <= n && align_video_fade::FindConfirmedPlateauStart(
												   std::span<double const>(vis).subspan(b), platform,
												   kFadePlateauSamples) == 0)
			++b;
		if (b + kFadePlateauSamples > n)
			return edge; // plateau reaches the end: no ramp was observed

		// Ramp start: the first below-band sample inside the failing window.
		double const plateau_floor = platform.level - platform.level_band;
		int r = b;
		for (int i = b; i < b + kFadePlateauSamples && i < n; ++i)
			if (vis[i] < plateau_floor) {
				r = i;
				break;
			}
		// The plateau tail handed to the fit must itself be confirmed.
		if (r < kFadePlateauSamples || n - r < kFadeMinOutsideSamples)
			return edge;
		if (align_video_fade::FindConfirmedPlateauStart(
				std::span<double const>(vis).subspan(
					r - kFadePlateauSamples, kFadePlateauSamples),
				platform, kFadePlateauSamples) != 0)
			return edge;

		int low_run = 0;
		int outside_end = r; // one past the last collected outside index
		for (int i = r; i < n && i - r < kFadeMaxFitFrames; ++i) {
			++outside_end;
			if (vis[i] <= platform.level * kFadeLowSignalFraction)
				++low_run;
			else
				low_run = 0;
			if (low_run >= kFadeLowSignalConfirmation)
				break;
		}
		if (outside_end - r < kFadeMinOutsideSamples)
			return edge;

		std::vector<double> window;
		std::vector<int> window_frames;
		window.reserve(size_t(outside_end - r + kFadePlateauSamples));
		window_frames.reserve(window.capacity());
		for (int i = outside_end - 1; i >= r; --i) {
			window.push_back(vis[i]);
			window_frames.push_back(frames[i]);
		}
		for (int i = r - 1; i >= r - kFadePlateauSamples; --i) {
			window.push_back(vis[i]);
			window_frames.push_back(frames[i]);
		}

		auto const fit = align_video_fade::FitVisibilityCurve(
			window, kFadePlateauSamples);
		if (!fit.detected)
			return edge;
		edge.detected = true;
		edge.outer_frame = window_frames[size_t(fit.outer_index)];
		edge.full_visibility_frame = window_frames[size_t(fit.inner_index)];
		edge.confidence = fit.confidence;
		return edge;
	};

	interval.fade_out = extract_direction(+1);
	interval.fade_in = extract_direction(-1);
	return interval;
}

int MotionTrackSession::SearchRadiusForRoi() const {
	if (domains_.search_radius_override > 0)
		return std::clamp(domains_.search_radius_override, 4, 256);
	int const max_side = std::max(roi_.w, roi_.h);
	return int(EffectiveRadius(max_side));
}

} // namespace aegisub::motion_track
