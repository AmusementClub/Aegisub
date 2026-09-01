#pragma once

// Coarse-to-fine translation estimation: a two-level spatial NCC pyramid
// (half-resolution scan over the search radius, then a full-resolution
// +-refine_radius refinement) with parabolic subpixel interpolation.
//
// Not implemented here (session responsibility): consecutive-failure stop and
// the velocity prior of the search center — both need per-direction history
// that outlives a single Step call and must survive Bidirectional's two
// independent cursors sharing one backend instance.

#include "ncc.h"
#include "types.h"

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace aegisub::motion_track {

struct TranslationTrackerConfig {
	double ncc_min = 0.55;
	double residual_max = 22.0;
	/// +- around the level-1 guess for the full-resolution refinement pass.
	int refine_radius = 3;
	/// Reject a step whose NCC surface has a competing local maximum
	/// displaced well beyond the accepted peak's shoulder and scoring at
	/// least ambiguity_ratio of it — the repetitive-texture lock-on trap.
	/// 0.9: genuine periodic repetitions score ~1.0, while the secondary
	/// lobes small smooth templates throw off stay below it.
	/// Costs one extra pass over the coarse scan window.
	bool ambiguity_check = true;
	double ambiguity_ratio = 0.9;
	/// Slow template refresh: after a step whose NCC reaches
	/// refresh_min_ncc, blend the matched image window into the template by
	/// refresh_alpha, so gradual appearance changes (lighting, focus, pose)
	/// do not accumulate into drift while the score still passes. Off by
	/// default: direct backend users (tests) expect a frozen template.
	bool template_refresh = false;
	double refresh_alpha = 0.05;
	double refresh_min_ncc = 0.85;
	/// Occlusion-robust acceptance: when the plain whole-template NCC falls
	/// below ncc_min, relocate the match by 8x8-block displacement consensus
	/// and rescore it on the pixels whose per-pixel difference survives a
	/// robust noise gate (median + 3 * 1.4826 * MAD) before failing the
	/// step. A partly occluded target then keeps tracking on its visible
	/// part instead of tripping NccLow; genuinely bad matches still fail
	/// because their differences are uniformly high.
	/// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
	/// (ISC license).
	bool robust_inlier_scoring = true;
	/// Upper bound on the template-area fraction the rescore may reject; a
	/// match needing more than this is treated as lost (NccLow), not
	/// occluded.
	double max_occlusion_fraction = 0.5;
	/// Held/duplicate-frame pre-check: before the pyramid search, compare
	/// the previous accepted match window against the same absolute region
	/// of the incoming frame. An exact (signature-equal) or near-identical
	/// (mean |difference| within the encoder-dither gate) frame keeps the
	/// previous integer offset with zero subpixel delta instead of
	/// searching, so luma noise on static content cannot produce spurious
	/// sub-pixel jitter, drift, or false NccLow/ResidualHigh failures.
	/// True motion changes the window far beyond the noise gate and still
	/// runs the full search; the first Step after Reset has no previous
	/// match and never takes the fast path. On by default.
	/// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
	/// (ISC license).
	bool held_frame_check = true;
	/// Fade detection, phase A (tracking robustness only): a global fade
	/// dims the tracked target without moving it, and zero-mean NCC is
	/// invariant to a*x+b brightness transforms, so the fade is observable
	/// only as a collapse of the least-squares contrast slope between the
	/// seed template and the window at the last accepted position. Frames
	/// whose plain match trips the NCC or residual gate first pass the fade
	/// gate: when the measured window is a certified scaled copy of the
	/// template (plain NCC >= 0.85 -- a fade is exactly a*x+b, so its
	/// correlation survives to ~1.0, while occlusion or misalignment
	/// collapses it well below) with slope <= fade_enter_slope, the step is
	/// fade-held: Ok at the last accepted position, fade_visibility =
	/// slope, instead of failing -- so fade frames can no longer kill the
	/// arm through consecutive failures. While fading, the held position is
	/// re-probed by a full search every fade_probe_interval frames: a probe
	/// measuring slope >= fade_exit_slope resumes normal tracking
	/// (hysteresis -- only a probe may end the hold), and a probe finding a
	/// confident match displaced more than ~2 px while the slope is still
	/// low reports the target lost (NccLow) rather than teleporting to it.
	/// There is deliberately no fixed maximum fade duration. Phase B
	/// (converting the recorded visibility curve into \fad timing via
	/// align_video_fade, session + apply plan) reuses the same curve: every
	/// accepted Ok step reports the contrast slope measured at its accepted
	/// match position, so -1.0 survives only where measurement is impossible
	/// or this flag is off. On by default.
	bool fade_detection = true;
	/// Contrast slope (relative to the seed template) at or below which a
	/// gated frame counts as faded.
	double fade_enter_slope = 0.6;
	/// Contrast slope a recovery probe must reach before tracking resumes;
	/// above fade_enter_slope by design, so single-frame slope spikes
	/// during a hold cannot exit early.
	double fade_exit_slope = 0.85;
	/// Fade-held frames between full-search probes of the held position
	/// (values below 1 are treated as 1: probe every frame).
	int fade_probe_interval = 8;
};

struct TrackerSeed {
	TrackModel model = TrackModel::Translation;
	RoiRect roi;
	GrayView template_gray; // valid only during the Reset call; Reset must copy
	int seed_frame = -1;
};

class TrackerBackend {
public:
	virtual ~TrackerBackend() = default;

	virtual TrackModel Model() const = 0;

	// Valid for the backend's lifetime; snapshot publishers must copy.
	virtual std::string_view Name() const noexcept = 0;

	// Copies template pixels into backend-owned storage. Non-Translation
	// models return Unsupported. Invalid/degenerate views return InvalidInput.
	virtual TrackStatus Reset(TrackerSeed const& seed) = 0;

	// One estimate per call; no unbounded retries inside the backend.
	virtual TrackStepResult Step(TrackStepRequest const& request) = 0;

	/// Signals that a new direction arm of a bidirectional run is about to
	/// step through the shared backend instance. Implementations must drop
	/// per-arm tracking state (last match, held window, fade state machine)
	/// while keeping the template and configuration, so the previous arm's
	/// history cannot leak into the new arm's first steps. The default does
	/// nothing (stateless backends).
	virtual void BeginDirectionArm() {}
};

class TranslationTrackerBackend final : public TrackerBackend {
public:
	TranslationTrackerBackend() = default;
	explicit TranslationTrackerBackend(TranslationTrackerConfig config);

	TrackModel Model() const override;
	std::string_view Name() const noexcept override;
	TrackStatus Reset(TrackerSeed const& seed) override;
	TrackStepResult Step(TrackStepRequest const& request) override;
	/// Clears the per-arm tracking state (held window, previous match,
	/// fade state machine) while keeping the template and config; see
	/// TrackerBackend::BeginDirectionArm.
	void BeginDirectionArm() override;

	/// Replaces the runtime-tunable config (e.g. the dialog enabling template
	/// refresh for the next Analyze). Does not touch the stored template.
	void SetConfig(TranslationTrackerConfig config) {
		config_ = std::move(config);
	}

	// Test observation only: FNV-1a over backend-owned template bytes
	// (0 for "no template"); not used by production code.
	std::uint64_t TemplateHash() const noexcept;

private:
	struct Estimate {
		bool ok = false;
		double local_dx = 0.0; // subpixel shift of template vs its anchor spot
		double local_dy = 0.0;
		double ncc = 0.0;
		/// Whole-template peak score before any occlusion rescore: the
		/// template-refresh gate must keep judging the plain match, never a
		/// partly-occluded one it would then blend into the template.
		double plain_ncc = 0.0;
		/// Median |template - window| at the accepted match (MadResidual's
		/// lower-median convention), held and fade-held steps included.
		double residual = 0.0;
		int match_ox = 0; // integer window offset of the match inside the image
		int match_oy = 0;
		TrackFailureReason failure = TrackFailureReason::None;
		/// Set when the fade gate inside the search claimed the frame: Step
		/// assembles the actual held result from backend state and ignores
		/// the offset fields.
		bool fade_held = false;
	};
	Estimate SpatialPyramidFallback(GrayView image, int anchor_x,
									int anchor_y, int radius_x, int radius_y,
									bool allow_fade_enter);
	void RefreshTemplate(GrayView image, int ox, int oy);

	void ClearTemplate();

	/// Measures the fade signal of the image window at (ox, oy) against the
	/// seed template in one scalar pass: plain NCC (-1.0 when undefined,
	/// e.g. a flat window) and the least-squares contrast slope (0.0 for a
	/// flat window, pinned 1.0 for a flat template; see ZeroMeanNccScalar).
	/// Returns false when the window lies outside the image.
	///
	/// `out_mean_abs`, when given, also receives the mean
	/// |template - window| difference. It costs a second pass over the
	/// window and no caller needs it since the published residual became
	/// MadResidual, so it is opt-in rather than a required out-param every
	/// call site has to declare a variable for.
	bool MeasureFadeSignal(GrayView image, int ox, int oy,
						   double& out_ncc, double& out_slope,
						   double *out_mean_abs = nullptr) const;

	/// Occlusion fallback for the pyramid search; see
	/// TranslationTrackerConfig::robust_inlier_scoring. When the
	/// whole-template peak is too weak, every 8x8 block of the seed
	/// template searches the prediction neighbourhood on its own and the
	/// block-displacement consensus (blocks agreeing within +-1 px, at
	/// least 1 - max_occlusion_fraction of the confident voters) picks the
	/// match. The match is then verified by a pixel-level inlier mask: on
	/// at least 1 - max_occlusion_fraction of the template pixels the
	/// per-pixel difference must sit inside a robust noise gate. On success
	/// fills the out params (offset, masked ncc and median, kept pixel
	/// mask) and returns true.
	bool RobustInlierSearch(GrayView image, int anchor_x, int anchor_y,
							int radius_x, int radius_y,
							int& out_offset_x, int& out_offset_y,
							double& out_ncc, double& out_median,
							std::vector<bool>& out_kept);

	TranslationTrackerConfig config_;
	bool has_template_ = false;
	int roi_w_ = 0;
	int roi_h_ = 0;
	std::vector<std::uint8_t> template_pixels_; // roi_w_ * roi_h_, row-packed
	// Held-frame pre-check state (config_.held_frame_check): the last
	// full-search match, in absolute storage coordinates, with a row-packed
	// copy of its pixels. Held steps deliberately leave it alone (comparing
	// against the last real match bounds the drift the noise gate could
	// accumulate); failed steps keep the last known good window. Reset
	// clears it. Like the template, it is only touched inside Reset/Step,
	// so the existing one-Step-at-a-time calling contract covers it.
	bool held_has_previous_ = false;
	int prev_match_abs_x_ = 0;
	int prev_match_abs_y_ = 0;
	std::vector<std::uint8_t> prev_window_; // roi_w_ * roi_h_, row-packed
	/// Last accepted ROI center in absolute storage coordinates, subpixel
	/// included. Fade-held steps re-report it verbatim, so the hold is
	/// exactly static and the session's velocity prior decays on natural
	/// zero deltas. Maintained with the held-check window state (full-search
	/// steps only); Reset clears it via held_has_previous_.
	double prev_center_abs_x_ = 0.0;
	double prev_center_abs_y_ = 0.0;
	// Fade state machine (config_.fade_detection): NotFaded -> Fading (a
	// gated frame certified the window at the held position as a scaled-down
	// copy of the template) -> NotFaded (a recovery probe measured slope >=
	// fade_exit_slope). Cleared in ClearTemplate.
	enum class FadePhase { NotFaded,
						   Fading };
	FadePhase fade_phase_ = FadePhase::NotFaded;
	/// Fade-held frames processed since the last full-search probe.
	int fade_frames_since_probe_ = 0;
	/// Last fade signal actually measured at the hold/match position (slope
	/// and the median |template - window| residual there), for steps whose
	/// own measurement is impossible (hold position outside the fetched
	/// crop): a hold must not publish an invented slope. Negative slope =
	/// nothing measured yet.
	double fade_last_slope_ = -1.0;
	double fade_last_residual_ = 0.0;
	/// Worst-case median |template - window| a certified scaled copy of this
	/// template can show while fading: max(median |t - med|, med, 255 - med)
	/// with med = median template value, computed once in Reset with the same
	/// median semantics as MadResidual. A copy at slope a fading toward a
	/// level L differs by (1 - a) * median |t - L|, which is maximal at the
	/// extrema L in {0, 255} (fade to black/white) with median |t - med| only
	/// covering fades toward the template's own median -- the residual the
	/// fade's ResidualHigh exemption must be armed against from slope
	/// 1 - residual_max / this.
	double template_mad_ = 0.0;
};

} // namespace aegisub::motion_track
