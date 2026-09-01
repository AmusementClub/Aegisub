#include "video_color_pick.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using Rgb = std::array<unsigned char, 3>;

// Full-range BT.601 YCbCr. Used only for grouping, edge analysis and the
// confidence score; every colour handed out stays in the source RGB space.
struct Ycbcr {
	double y = 0.0;
	double cb = 0.0;
	double cr = 0.0;
};

Ycbcr ToYcbcr(Rgb const& rgb) {
	double const r = rgb[0], g = rgb[1], b = rgb[2];
	return {
		0.299 * r + 0.587 * g + 0.114 * b,
		128.0 - 0.168736 * r - 0.331264 * g + 0.5 * b,
		128.0 + 0.5 * r - 0.418688 * g - 0.081312 * b,
	};
}

double Manhattan(Ycbcr const& a, Ycbcr const& b) {
	return std::abs(a.y - b.y) + std::abs(a.cb - b.cb) + std::abs(a.cr - b.cr);
}

double SquaredDistance(Ycbcr const& a, Ycbcr const& b) {
	double const dy = a.y - b.y, dcb = a.cb - b.cb, dcr = a.cr - b.cr;
	return dy * dy + dcb * dcb + dcr * dcr;
}

bool WithinTolerances(
	Ycbcr const& value,
	Ycbcr const& reference,
	int luma_tolerance,
	int chroma_tolerance) {
	return std::abs(value.y - reference.y) <= luma_tolerance && std::abs(value.cb - reference.cb) <= chroma_tolerance && std::abs(value.cr - reference.cr) <= chroma_tolerance;
}

unsigned char const *RowBytes(VideoFrame const& frame, int y) {
	auto const row = static_cast<size_t>(frame.flipped ? static_cast<int>(frame.height) - 1 - y : y);
	return frame.data.data() + row * frame.pitch;
}

Rgb PixelAt(VideoFrame const& frame, int x, int y) {
	unsigned char const *bytes = RowBytes(frame, y) + static_cast<size_t>(x) * 4;
	return {bytes[2], bytes[1], bytes[0]};
}

int ClampInt(int value, int lowest, int greatest) {
	return std::max(lowest, std::min(greatest, value));
}

agi::Color MakeColor(Rgb const& rgb) {
	return {rgb[0], rgb[1], rgb[2], 0};
}

Rgb ChannelStatistic(std::vector<Rgb> const& pixels, aegisub::color_pick::Statistic statistic) {
	if (pixels.empty())
		return {0, 0, 0};
	Rgb result{};
	for (int channel = 0; channel < 3; ++channel) {
		std::vector<int> values;
		values.reserve(pixels.size());
		for (auto const& pixel : pixels)
			values.push_back(pixel[channel]);
		if (statistic == aegisub::color_pick::Statistic::Median) {
			size_t const middle = values.size() / 2;
			std::nth_element(values.begin(), values.begin() + middle, values.end());
			result[channel] = static_cast<unsigned char>(values[middle]);
		}
		else {
			long long sum = 0;
			for (int value : values)
				sum += value;
			auto const count = static_cast<long long>(values.size());
			result[channel] =
				static_cast<unsigned char>((sum + count / 2) / count);
		}
	}
	return result;
}

struct WindowSample {
	Rgb rgb;
	Ycbcr ycbcr;
};

struct WindowBounds {
	int left;
	int top;
	int right;
	int bottom;

	int Width() const { return right - left + 1; }
	int Height() const { return bottom - top + 1; }
};

// The square window centred at (x, y), clamped to the frame's extent.
WindowBounds ClampedWindowBounds(int x, int y, int radius, int width, int height) {
	return {
		ClampInt(x - radius, 0, width - 1),
		ClampInt(y - radius, 0, height - 1),
		ClampInt(x + radius, 0, width - 1),
		ClampInt(y + radius, 0, height - 1),
	};
}

// Pixels of the clamped square window centred at (x, y), in scan order.
std::vector<WindowSample> WindowSamples(VideoFrame const& frame, int x, int y, int radius) {
	int const width = static_cast<int>(frame.width);
	int const height = static_cast<int>(frame.height);
	auto const bounds = ClampedWindowBounds(x, y, radius, width, height);

	std::vector<WindowSample> samples;
	samples.reserve(static_cast<size_t>(bounds.Width()) * bounds.Height());
	for (int sy = bounds.top; sy <= bounds.bottom; ++sy)
		for (int sx = bounds.left; sx <= bounds.right; ++sx) {
			samples.push_back({PixelAt(frame, sx, sy), {}});
			samples.back().ycbcr = ToYcbcr(samples.back().rgb);
		}
	return samples;
}

/// Split a small window into two dominant colour clusters with a fixed-seed
/// 2-means pass, used to judge whether the clicked point sits between two
/// stable blocks rather than on either of them.
struct ClusterPair {
	bool valid = false;
	std::array<Ycbcr, 2> mean{};
	/// Mean Manhattan deviation of the members around their cluster mean.
	std::array<double, 2> spread{};
	std::array<size_t, 2> count{};
	/// Channel medians of the member pixels; direct replacements for the seed.
	std::array<Rgb, 2> representative_rgb{};
};

ClusterPair SplitWindowClusters(std::vector<WindowSample> const& samples, double merge_threshold) {
	ClusterPair result;
	size_t const count = samples.size();
	if (count < 8)
		return result;

	// Initial means come from the extreme pixels along the widest YCbCr axis.
	struct Extreme {
		double value;
		size_t index;
	};
	std::array<Extreme, 3> lows{
		Extreme{std::numeric_limits<double>::infinity(), 0},
		Extreme{std::numeric_limits<double>::infinity(), 0},
		Extreme{std::numeric_limits<double>::infinity(), 0}};
	std::array<Extreme, 3> highs{
		Extreme{-std::numeric_limits<double>::infinity(), 0},
		Extreme{-std::numeric_limits<double>::infinity(), 0},
		Extreme{-std::numeric_limits<double>::infinity(), 0}};
	for (size_t i = 0; i < count; ++i) {
		for (int a = 0; a < 3; ++a) {
			double const v = a == 0   ? samples[i].ycbcr.y
							 : a == 1 ? samples[i].ycbcr.cb
									  : samples[i].ycbcr.cr;
			if (v < lows[a].value)
				lows[a] = {v, i};
			if (v > highs[a].value)
				highs[a] = {v, i};
		}
	}
	int best_axis = 0;
	double best_span = -1.0;
	for (int a = 0; a < 3; ++a) {
		double const span = highs[a].value - lows[a].value;
		if (span > best_span) {
			best_span = span;
			best_axis = a;
		}
	}
	// A narrow window holds one visual block; splitting would invent edges.
	if (!(best_span > merge_threshold))
		return result;

	std::array<Ycbcr, 2> means{
		samples[lows[best_axis].index].ycbcr,
		samples[highs[best_axis].index].ycbcr};
	std::vector<char> assignment(count, 0);
	for (int iteration = 0; iteration < 12; ++iteration) {
		bool changed = false;
		for (size_t i = 0; i < count; ++i) {
			char const target =
				SquaredDistance(samples[i].ycbcr, means[0]) <= SquaredDistance(samples[i].ycbcr, means[1]) ? 0 : 1;
			if (assignment[i] != target) {
				assignment[i] = target;
				changed = true;
			}
		}
		std::array<Ycbcr, 2> sums{};
		std::array<size_t, 2> counts{};
		for (size_t i = 0; i < count; ++i) {
			int const side = assignment[i];
			sums[side].y += samples[i].ycbcr.y;
			sums[side].cb += samples[i].ycbcr.cb;
			sums[side].cr += samples[i].ycbcr.cr;
			++counts[side];
		}
		for (int side = 0; side < 2; ++side) {
			if (!counts[side])
				continue;
			means[side] = {
				sums[side].y / counts[side],
				sums[side].cb / counts[side],
				sums[side].cr / counts[side]};
		}
		if (!changed)
			break;
	}

	result.count = {
		static_cast<size_t>(std::count(assignment.begin(), assignment.end(), static_cast<char>(0))),
		static_cast<size_t>(std::count(assignment.begin(), assignment.end(), static_cast<char>(1)))};
	if (!result.count[0] || !result.count[1])
		return {};

	result.valid = true;
	result.mean = means;

	std::array<Ycbcr, 2> deviations{};
	std::array<std::vector<Rgb>, 2> members;
	for (size_t i = 0; i < count; ++i) {
		int const side = assignment[i];
		deviations[side].y += std::abs(samples[i].ycbcr.y - means[side].y);
		deviations[side].cb += std::abs(samples[i].ycbcr.cb - means[side].cb);
		deviations[side].cr += std::abs(samples[i].ycbcr.cr - means[side].cr);
		members[side].push_back(samples[i].rgb);
	}
	for (int side = 0; side < 2; ++side) {
		result.spread[side] =
			(deviations[side].y + deviations[side].cb + deviations[side].cr) / static_cast<double>(result.count[side]);
		result.representative_rgb[side] =
			ChannelStatistic(members[side], aegisub::color_pick::Statistic::Median);
	}
	return result;
}

struct RegionGeometry {
	int min_x = 0;
	int min_y = 0;
	int max_x = -1;
	int max_y = -1;
};

void NoteBounds(RegionGeometry& box, int px, int py) {
	if (box.max_x < box.min_x) {
		box.min_x = box.max_x = px;
		box.min_y = box.max_y = py;
		return;
	}
	box.min_x = std::min(box.min_x, px);
	box.max_x = std::max(box.max_x, px);
	box.min_y = std::min(box.min_y, py);
	box.max_y = std::max(box.max_y, py);
}

bool BoundsFitExtent(RegionGeometry const& box, int px, int py, int extent) {
	int const lo_x = std::min(box.min_x, px);
	int const hi_x = std::max(box.max_x, px);
	int const lo_y = std::min(box.min_y, py);
	int const hi_y = std::max(box.max_y, py);
	return hi_x - lo_x <= extent && hi_y - lo_y <= extent;
}

/// Score how well the picked region separates from its surrounding ring:
/// strong region/ring contrast with little internal spread scores high.
double ComputeConfidence(
	VideoFrame const& frame,
	std::vector<char> const& member_mask,
	int width,
	int height,
	RegionGeometry const& box,
	Ycbcr const& reference,
	double spread) {
	constexpr int padding = 4;
	int const left = ClampInt(box.min_x - padding, 0, width - 1);
	int const right = ClampInt(box.max_x + padding, 0, width - 1);
	int const top = ClampInt(box.min_y - padding, 0, height - 1);
	int const bottom = ClampInt(box.max_y + padding, 0, height - 1);

	double total_distance = 0.0;
	int ring_count = 0;
	for (int ry = top; ry <= bottom; ++ry) {
		for (int rx = left; rx <= right; ++rx) {
			auto const linear = static_cast<size_t>(ry) * width + rx;
			if (member_mask[linear])
				continue;
			total_distance += Manhattan(ToYcbcr(PixelAt(frame, rx, ry)), reference);
			++ring_count;
		}
	}
	if (!ring_count)
		return 0.5;
	double const contrast = total_distance / ring_count;
	return std::clamp(contrast / (contrast + 2.0 * spread + 4.0), 0.0, 1.0);
}

} // namespace

namespace aegisub::color_pick {

Result PickColor(VideoFrame const& frame, int x, int y, Options const& options) {
	int const width = static_cast<int>(frame.width);
	int const height = static_cast<int>(frame.height);
	if (frame.data.empty() || width <= 0 || height <= 0 || frame.pitch < static_cast<size_t>(width) * 4 || frame.data.size() < frame.pitch * frame.height)
		return {};

	Options opt = options;
	opt.seed_radius = std::max(0, opt.seed_radius);
	opt.fallback_radius = std::max(0, opt.fallback_radius);
	opt.luma_tolerance = std::max(0, opt.luma_tolerance);
	opt.chroma_tolerance = std::max(0, opt.chroma_tolerance);
	opt.max_region_pixels = std::max(1, opt.max_region_pixels);
	opt.max_region_extent = std::max(1, opt.max_region_extent);

	Result result;
	int const clamped_x = ClampInt(x, 0, width - 1);
	int const clamped_y = ClampInt(y, 0, height - 1);
	bool const inside_frame = x >= 0 && y >= 0 && x < width && y < height;

	if (!inside_frame) {
		auto const bounds = ClampedWindowBounds(clamped_x, clamped_y, opt.fallback_radius, width, height);
		auto const samples = WindowSamples(frame, clamped_x, clamped_y, opt.fallback_radius);
		std::vector<Rgb> rgbs;
		rgbs.reserve(samples.size());
		for (auto const& sample : samples)
			rgbs.push_back(sample.rgb);
		result.color = MakeColor(ChannelStatistic(rgbs, opt.statistic));
		result.pixels = static_cast<int>(rgbs.size());
		result.bbox_w = bounds.Width();
		result.bbox_h = bounds.Height();
		result.fallback = true;
		return result;
	}

	// Seed colour: per-channel statistic of the seed window.
	auto const seed_samples = WindowSamples(frame, clamped_x, clamped_y, opt.seed_radius);
	std::vector<Rgb> seed_rgbs;
	seed_rgbs.reserve(seed_samples.size());
	for (auto const& sample : seed_samples)
		seed_rgbs.push_back(sample.rgb);
	Rgb reference_rgb = ChannelStatistic(seed_rgbs, opt.statistic);
	Ycbcr reference = ToYcbcr(reference_rgb);

	// Edge analysis: judge whether the seed sits on the anti-aliased seam of
	// two stable blocks and, if so, snap onto the nearer block.
	auto const edge_samples = WindowSamples(
		frame, clamped_x, clamped_y, std::max(4, opt.seed_radius));
	double const combined_tolerance = static_cast<double>(opt.luma_tolerance) + 2.0 * opt.chroma_tolerance;
	auto clusters = SplitWindowClusters(edge_samples, combined_tolerance);
	if (clusters.valid) {
		size_t const minimum_members = std::max<size_t>(4, edge_samples.size() / 8);
		double const spread_limit = combined_tolerance * 0.75;
		auto acceptable = [&](int side) {
			return clusters.count[side] >= minimum_members && clusters.spread[side] <= spread_limit;
		};
		bool const seed_on_block_0 =
			Manhattan(reference, clusters.mean[0]) <= combined_tolerance;
		bool const seed_on_block_1 =
			Manhattan(reference, clusters.mean[1]) <= combined_tolerance;
		bool const distinct_blocks =
			Manhattan(clusters.mean[0], clusters.mean[1]) > combined_tolerance;
		if (distinct_blocks && !seed_on_block_0 && !seed_on_block_1 && (acceptable(0) || acceptable(1))) {
			int chosen = -1;
			if (acceptable(0) && acceptable(1)) {
				double const distance_0 = Manhattan(reference, clusters.mean[0]);
				double const distance_1 = Manhattan(reference, clusters.mean[1]);
				chosen = distance_0 != distance_1
							 ? (distance_0 < distance_1 ? 0 : 1)
							 : (clusters.count[0] >= clusters.count[1] ? 0 : 1);
			}
			else {
				chosen = acceptable(0) ? 0 : 1;
			}
			reference_rgb = clusters.representative_rgb[chosen];
			reference = clusters.mean[chosen];
			result.edge_snapped = true;
		}
	}

	// Flood fill from every seed-patch pixel matching the reference colour.
	auto const min_start_count = static_cast<size_t>(
		std::max(1, ((opt.seed_radius * 2 + 1) * (opt.seed_radius * 2 + 1)) / 2));
	std::vector<char> enqueued(static_cast<size_t>(width) * height, 0);
	std::vector<size_t> pending;
	pending.reserve(min_start_count * 2);

	auto try_enqueue = [&](int px, int py) {
		auto& slot = enqueued[static_cast<size_t>(py) * width + px];
		if (slot)
			return;
		if (!WithinTolerances(ToYcbcr(PixelAt(frame, px, py)), reference,
							  opt.luma_tolerance, opt.chroma_tolerance))
			return;
		slot = 1;
		pending.push_back(static_cast<size_t>(py) * width + px);
	};

	int const left_bound = ClampInt(clamped_x - opt.seed_radius, 0, width - 1);
	int const right_bound = ClampInt(clamped_x + opt.seed_radius, 0, width - 1);
	int const top_bound = ClampInt(clamped_y - opt.seed_radius, 0, height - 1);
	int const bottom_bound = ClampInt(clamped_y + opt.seed_radius, 0, height - 1);
	for (int sy = top_bound; sy <= bottom_bound; ++sy)
		for (int sx = left_bound; sx <= right_bound; ++sx)
			try_enqueue(sx, sy);

	std::vector<Rgb> region_rgbs;
	region_rgbs.reserve(pending.capacity());
	RegionGeometry box;
	while (!pending.empty()) {
		size_t const index = pending.back();
		pending.pop_back();
		int const px = static_cast<int>(index % width);
		int const py = static_cast<int>(index / width);

		NoteBounds(box, px, py);
		region_rgbs.push_back(PixelAt(frame, px, py));
		if (static_cast<int>(region_rgbs.size()) >= opt.max_region_pixels) {
			result.capped = true;
			break;
		}

		constexpr int steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
		for (auto const& step : steps) {
			int const nx = px + step[0];
			int const ny = py + step[1];
			if (nx < 0 || ny < 0 || nx >= width || ny >= height)
				continue;
			if (!BoundsFitExtent(box, nx, ny, opt.max_region_extent)) {
				result.capped = true;
				continue;
			}
			try_enqueue(nx, ny);
		}
	}

	// Degenerate region -> plain local median around the clicked point.
	if (region_rgbs.size() < min_start_count) {
		auto const bounds = ClampedWindowBounds(clamped_x, clamped_y, opt.fallback_radius, width, height);
		auto const samples = WindowSamples(frame, clamped_x, clamped_y, opt.fallback_radius);
		std::vector<Rgb> rgbs;
		rgbs.reserve(samples.size());
		for (auto const& sample : samples)
			rgbs.push_back(sample.rgb);
		result.color = MakeColor(ChannelStatistic(rgbs, opt.statistic));
		result.pixels = static_cast<int>(rgbs.size());
		result.bbox_w = bounds.Width();
		result.bbox_h = bounds.Height();
		result.fallback = true;
		return result;
	}

	result.color = MakeColor(ChannelStatistic(region_rgbs, opt.statistic));
	result.pixels = static_cast<int>(region_rgbs.size());
	result.bbox_w = box.max_x - box.min_x + 1;
	result.bbox_h = box.max_y - box.min_y + 1;

	double spread_total = 0.0;
	for (auto const& rgb : region_rgbs)
		spread_total += Manhattan(ToYcbcr(rgb), reference);
	result.confidence = ComputeConfidence(
		frame, enqueued, width, height, box, reference,
		spread_total / static_cast<double>(region_rgbs.size()));
	return result;
}

std::vector<agi::Color> ExtractZoomRegion(VideoFrame const& frame, int x, int y, int radius) {
	std::vector<agi::Color> region;
	int const width = static_cast<int>(frame.width);
	int const height = static_cast<int>(frame.height);
	if (radius < 0 || frame.data.empty() || width <= 0 || height <= 0 || frame.pitch < static_cast<size_t>(width) * 4 || frame.data.size() < frame.pitch * frame.height)
		return region;

	int const extent = 2 * radius + 1;
	region.resize(static_cast<size_t>(extent) * extent);
	for (int dy = -radius; dy <= radius; ++dy) {
		for (int dx = -radius; dx <= radius; ++dx) {
			// Edge cells read the clamped border pixel so the grid stays square
			// and its centre is always the pixel under the pointer.
			Rgb const rgb = PixelAt(
				frame,
				ClampInt(x + dx, 0, width - 1),
				ClampInt(y + dy, 0, height - 1));
			region[static_cast<size_t>(dy + radius) * extent + (dx + radius)] =
				agi::Color(rgb[0], rgb[1], rgb[2], 0);
		}
	}
	return region;
}

std::pair<int, int> MapDisplayPointToStorage(SourceFrameGeometry const& geometry, double x, double y) {
	auto const display = GetSourceFrameDisplayOutputRect(geometry);
	if (display.width <= 0 || display.height <= 0)
		return {-1, -1};
	return {
		display.x + std::clamp(static_cast<int>(std::llround(x)), 0, display.width - 1),
		display.y + std::clamp(static_cast<int>(std::llround(y)), 0, display.height - 1),
	};
}

} // namespace aegisub::color_pick
