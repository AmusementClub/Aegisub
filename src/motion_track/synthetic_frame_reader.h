#pragma once

// Deterministic synthetic translation scene + MotionFrameReader for core
// tests. No video provider, no wx. The object texture translates subpixel-
// smoothly (bilinear) by velocity per frame; background is a deterministic
// hash texture so tests are reproducible without stored buffers.

#include "frame_reader.h"

#include <cstdint>

namespace aegisub::motion_track {

class SyntheticTranslationScene {
public:
	struct Config {
		int width = 0;            // scene size in pixels
		int height = 0;
		int frame_count = 0;      // valid frame domain for FetchGray
		RoiRect object_at_frame0; // object footprint at frame 0 (w/h > 1)
		double velocity_x = 0.0;  // storage px per frame, may be fractional
		double velocity_y = 0.0;
		std::uint64_t seed = 0x9E3779B97F4A7C15ull;
	};

	explicit SyntheticTranslationScene(Config config);

	int Width() const { return config_.width; }
	int Height() const { return config_.height; }
	int FrameCount() const { return config_.frame_count; }

	// Analytic object center in pixel-center convention at a given frame.
	double ObjectCenterX(int frame) const;
	double ObjectCenterY(int frame) const;

	// Gray value of one scene pixel; deterministic across calls.
	std::uint8_t Pixel(int x, int y, int frame) const;

private:
	std::uint8_t Background(int x, int y) const;
	std::uint8_t ObjectTexture(double u, double v) const;

	Config config_;
};

class SyntheticFrameReader final : public MotionFrameReader {
public:
	explicit SyntheticFrameReader(SyntheticTranslationScene scene);

	// Crops the requested ROI from the scene. Out-of-bounds pixels follow the
	// production rule: fill with the mean gray of the in-bounds intersection;
	// when the intersection is empty fill with neutral 128. Status is Ok even
	// then — only invalid frame indices yield FrameUnavailable.
	FrameReadResult FetchGray(int frame, RoiRect roi, GrayPatch& out) override;

	SyntheticTranslationScene const& Scene() const { return scene_; }

private:
	SyntheticTranslationScene scene_;
};

} // namespace aegisub::motion_track
