#pragma once

// Similarity (translation + rotation + uniform scale) tracker backend.
// Forward-additive Gauss-Newton intensity alignment (ECC family) over the
// gray ROI: parameters (tx, ty, rotation, scale) warp the frozen seed
// template onto the search image; each iteration samples the image and its
// gradients bilinearly at the warped positions and solves the 4x4 normal
// equations. The template is the seed snapshot, so the estimated pose is
// seed-relative and never accumulates integration drift; per-frame motion is
// small, and the session supplies the previous accepted pose as the starting
// point via TrackStepRequest::init_rotation/init_scale.

#include "translation_backend.h"
#include "types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace aegisub::motion_track {

struct SimilarityTrackerConfig {
	double ncc_min = 0.55;
	double residual_max = 22.0;
	int max_iterations = 30;
	/// Convergence: stop once every parameter update is below this.
	double step_tolerance = 1e-7;
	/// Sanity gates on the per-step pose change (vs the init pose): a bigger
	/// jump means the refinement ran away or locked onto something else.
	double max_rotation_per_step = 0.35; // ~20 degrees
	double min_scale_ratio_per_step = 0.75;
	double max_scale_ratio_per_step = 1.333;
	/// Trust region: per-iteration update clamps that keep the linearization
	/// valid without line searching.
	double max_iteration_translation = 20.0; // px
	double max_iteration_rotation = 0.25;    // rad
	double max_iteration_log_scale = 0.25;
	/// Subsample the template when its area exceeds this many samples.
	int max_samples = 16384;
	/// Occlusion-robust final acceptance: when the plain whole-template NCC
	/// falls below ncc_min, rescore on 8x8 template blocks whose residual
	/// survives a robust median + 3 * 1.4826 * MAD cutoff (the estimation
	/// side is already robust via the IRLS Tukey pass; this gate is what a
	/// partly occluded frame otherwise trips). Same mechanism as
	/// TranslationTrackerConfig::robust_inlier_scoring.
	/// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
	/// (ISC license).
	bool robust_inlier_scoring = true;
	/// Upper bound on the block fraction the rescore may reject.
	double max_occlusion_fraction = 0.5;
};

class SimilarityTrackerBackend final : public TrackerBackend {
public:
	SimilarityTrackerBackend() = default;
	explicit SimilarityTrackerBackend(SimilarityTrackerConfig config);

	TrackModel Model() const override;
	std::string_view Name() const noexcept override;
	TrackStatus Reset(TrackerSeed const& seed) override;
	TrackStepResult Step(TrackStepRequest const& request) override;

	std::uint64_t TemplateHash() const noexcept;

private:
	SimilarityTrackerConfig config_;
	bool has_template_ = false;
	int roi_w_ = 0;
	int roi_h_ = 0;
	std::vector<std::uint8_t> template_pixels_; // roi_w_ * roi_h_, row-packed

	void ClearTemplate();
};

} // namespace aegisub::motion_track
