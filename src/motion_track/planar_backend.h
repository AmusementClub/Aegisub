#pragma once

#include "translation_backend.h"

#include <vector>

namespace aegisub::motion_track {

struct PlanarTrackerConfig {
	double ncc_min = 0.65;
	double residual_max = 22.0;
	int max_iterations = 35;
	int max_samples = 16384;
	double max_corner_step = 48.0;
	double min_area_ratio_per_step = 0.5;
	double max_area_ratio_per_step = 2.0;
	double max_occlusion_fraction = 0.35;
};

// Direct grayscale registration against the frozen seed, with six affine or
// eight homography parameters. Normalized template coordinates condition the
// normal equations; bounded Gauss-Newton steps and Tukey weights keep partial
// occlusion from dominating the fit. This is a local, frame-to-frame tracker.
class PlanarTrackerBackend final : public TrackerBackend {
	public:
	explicit PlanarTrackerBackend(TrackModel model, PlanarTrackerConfig config = {});
	[[nodiscard]] TrackModel Model() const override;
	[[nodiscard]] std::string_view Name() const noexcept override;
	TrackStatus Reset(TrackerSeed const& seed) override;
	TrackStepResult Step(TrackStepRequest const& request) override;

	private:
	TrackModel model_;
	PlanarTrackerConfig config_;
	int width_ = 0;
	int height_ = 0;
	std::vector<std::uint8_t> template_pixels_;
};

} // namespace aegisub::motion_track
