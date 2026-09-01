#include "synthetic_frame_reader.h"

#include <algorithm>
#include <utility>
#include <cmath>

namespace aegisub::motion_track {

SyntheticTranslationScene::SyntheticTranslationScene(Config config)
	: config_(config) {
	config_.object_at_frame0.w = std::max(1, config_.object_at_frame0.w);
	config_.object_at_frame0.h = std::max(1, config_.object_at_frame0.h);
}


std::uint8_t SyntheticTranslationScene::Background(int x, int y) const {
	// Smooth band-limited background plus a slow linear ramp: real footage
	// is spatially non-stationary, and the ramp keeps large-radius searches
	// from locking onto quasi-periodic false matches.
	double const p1 = double(config_.seed % 628ull) / 100.0;
	double const p2 = double((config_.seed >> 10) % 628ull) / 100.0;
	double const s = std::sin(x * 0.17 + p1) * std::cos(y * 0.19 + p2)
	               + 0.4 * std::sin((x + y) * 0.11 + p1);
	double const ramp = 0.05 * double(x - y);
	return static_cast<std::uint8_t>(
		std::clamp(110.0 + 42.0 * s + ramp, 0.0, 255.0));
}

std::uint8_t SyntheticTranslationScene::ObjectTexture(double u, double v) const {
	// Multi-frequency smooth texture: low frequencies survive interpolation
	// (subpixel accuracy), the mid-frequency term gives phase correlation
	// enough spectral breadth for a sharp, unambiguous peak.
	double const p1 = double(config_.seed % 628ull) / 100.0;
	double const p2 = double((config_.seed >> 10) % 628ull) / 100.0;
	double const p3 = double((config_.seed >> 20) % 628ull) / 100.0;
	double const s = std::sin(u * 0.55 + p1) * std::cos(v * 0.43 + p2)
	               + 0.6 * std::sin((u - v) * 0.27 + p2)
	               + 0.45 * std::sin(u * 1.9 + p3) * std::cos(v * 1.7 - p1)
	               + 0.35 * std::sin(std::sqrt(u * u + v * v) * 0.21 + p1);
	return static_cast<std::uint8_t>(
		std::clamp(128.0 + 52.0 * (s / 2.4), 0.0, 255.0));
}

double SyntheticTranslationScene::ObjectCenterX(int frame) const {
	double const left = config_.object_at_frame0.x
	                  + config_.velocity_x * frame;
	return left + (config_.object_at_frame0.w - 1) / 2.0;
}

double SyntheticTranslationScene::ObjectCenterY(int frame) const {
	double const top = config_.object_at_frame0.y
	                  + config_.velocity_y * frame;
	return top + (config_.object_at_frame0.h - 1) / 2.0;
}

std::uint8_t SyntheticTranslationScene::Pixel(int x, int y, int frame) const {
	// Object footprint top-left at this frame (fractional).
	double const left = config_.object_at_frame0.x + config_.velocity_x * frame;
	double const top = config_.object_at_frame0.y + config_.velocity_y * frame;
	double const w = static_cast<double>(config_.object_at_frame0.w);
	double const h = static_cast<double>(config_.object_at_frame0.h);

	// Exact coverage of this pixel by the object rectangle; edge pixels
	// blend object texture and background proportionally, which keeps the
	// rendered motion temporally consistent at subpixel offsets.
	double const ov_x =
		std::min(left + w, double(x + 1)) - std::max(left, double(x));
	double const ov_y =
		std::min(top + h, double(y + 1)) - std::max(top, double(y));
	double const coverage = std::clamp(ov_x, 0.0, 1.0)
	                      * std::clamp(ov_y, 0.0, 1.0);
	if (coverage <= 0.0)
		return Background(x, y);

	// Texture coordinate relative to the object's top-left corner; the
	// fractional part carries the subpixel translation.
	double const tex = ObjectTexture(double(x) - left, double(y) - top);
	if (coverage >= 1.0)
		return static_cast<std::uint8_t>(tex);
	double const bg = Background(x, y);
	return static_cast<std::uint8_t>(
		std::clamp(coverage * tex + (1.0 - coverage) * bg, 0.0, 255.0));
}

SyntheticFrameReader::SyntheticFrameReader(SyntheticTranslationScene scene)
	: scene_(std::move(scene)) {}

FrameReadResult SyntheticFrameReader::FetchGray(
	int frame, RoiRect roi, GrayPatch& out) {
	if (frame < 0 || frame >= scene_.FrameCount()) {
		return FrameReadResult{FrameReadStatus::FrameUnavailable,
		                       "synthetic frame out of range"};
	}

	int const w = std::max(roi.w, 0);
	int const h = std::max(roi.h, 0);

	// Mean of the in-bounds intersection for border fill; neutral 128 when
	// the ROI misses the frame entirely (production parity rule).
	long long sum = 0;
	long long count = 0;
	for (int y = std::max(roi.y, 0);
	     y < std::min(roi.y + h, scene_.Height()); ++y) {
		for (int x = std::max(roi.x, 0);
		     x < std::min(roi.x + w, scene_.Width()); ++x) {
			sum += scene_.Pixel(x, y, frame);
			++count;
		}
	}
	std::uint8_t fill = count > 0
	    ? static_cast<std::uint8_t>((sum + count / 2) / count)
	    : 128;

	out.frame = frame;
	out.origin_x = roi.x;
	out.origin_y = roi.y;
	out.width = w;
	out.height = h;
	out.stride = w > 0 ? w : 0;
	out.gray.assign(size_t(w) * size_t(h), fill);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			int sx = roi.x + x;
			int sy = roi.y + y;
			bool inside = sx >= 0 && sy >= 0 && sx < scene_.Width()
			           && sy < scene_.Height();
			out.gray[size_t(y) * size_t(w) + size_t(x)]
			    = inside ? scene_.Pixel(sx, sy, frame) : fill;
		}
	}
	return FrameReadResult{FrameReadStatus::Ok, {}};
}

} // namespace aegisub::motion_track
