#include "ncc.h"

#ifdef AEGISUB_WITH_HIGHWAY
#include "../simd/motion_track_ncc_simd.h"
#endif

#include <algorithm>
#include <cmath>

namespace aegisub::motion_track {

namespace {

// Raw int64 sums of a gray view. Exact for any 8-bit view: each accumulator
// is bounded by N * 65025 <= 1.7e10 for the slice's frozen size caps.
void AccumulateViewSums(GrayView view, long long& sum, long long& sum_sq) {
	sum = 0;
	sum_sq = 0;
	for (int y = 0; y < view.height; ++y) {
		auto const* row = view.data + int64_t(y) * view.stride;
		for (int x = 0; x < view.width; ++x) {
			int const v = row[x];
			sum += v;
			sum_sq += v * v;
		}
	}
}

} // namespace

namespace {

// Running-best update shared by both tie-break modes.
// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
// (ISC license): a strictly better score
// (margin 1e-12) always wins; otherwise a score within 1e-12 of the running
// best wins only when it is strictly nearer to the tie-break center, so
// identical-scoring candidates cannot creep toward the scan origin one frame
// at a time. best_ncc tracks the maximum score seen, mirroring the upstream
// rule where the reported correlation is the max while the offset may come
// from an equal-scoring nearer candidate.
struct NccBestState {
	double best_ncc = -2.0;
	BestNccResult best;
	NccTieBreak tie;

	void Consider(double ncc, int ox, int oy) {
		bool const better = !best.found || ncc > best_ncc + 1e-12;
		bool const tie_nearer = best.found && tie.enabled && ncc > best_ncc - 1e-12 && (ox - tie.center_x) * (ox - tie.center_x) + (oy - tie.center_y) * (oy - tie.center_y) < (best.offset_x - tie.center_x) * (best.offset_x - tie.center_x) + (best.offset_y - tie.center_y) * (best.offset_y - tie.center_y);
		if (!better && !tie_nearer)
			return;
		if (ncc > best_ncc)
			best_ncc = ncc;
		best.ncc = best_ncc;
		best.offset_x = ox;
		best.offset_y = oy;
		best.found = true;
	}
};

} // namespace

bool ZeroMeanNccScalar(
	GrayView templ, GrayView image, int ox, int oy, double& out,
	double *out_slope) {
	if (!templ.data || !image.data || templ.width <= 0 || templ.height <= 0
	    || templ.stride < templ.width || image.stride < image.width)
		return false;
	if (ox < 0 || oy < 0 || ox + templ.width > image.width
	    || oy + templ.height > image.height)
		return false;

	long long sum_t = 0;
	long long sum_t2 = 0;
	AccumulateViewSums(templ, sum_t, sum_t2);

	long long sum_i = 0;
	long long sum_i2 = 0;
	long long sum_ti = 0;
	for (int y = 0; y < templ.height; ++y) {
		auto const* trow = templ.data + int64_t(y) * templ.stride;
		auto const* irow =
			image.data + int64_t(oy + y) * image.stride + ox;
		for (int x = 0; x < templ.width; ++x) {
			int const tv = trow[x];
			int const iv = irow[x];
			sum_i += iv;
			sum_i2 += iv * iv;
			sum_ti += tv * iv;
		}
	}

	long long const n = int64_t(templ.width) * templ.height;
	long long const num = n * sum_ti - sum_t * sum_i;
	long long const factor_t = n * sum_t2 - sum_t * sum_t;
	long long const factor_i = n * sum_i2 - sum_i * sum_i;
	if (factor_t < 0 || factor_i < 0)
		return false; // cannot happen for real inputs; guards UB on misuse
	if (out_slope) {
		// Slope of the least-squares brightness line window = a*templ + b:
		// num and factor_t are the exact int64 regression sums, so no second
		// pass is needed. Filled before the flat-side early return below --
		// the slope stays meaningful when the NCC does not (see ncc.h).
		*out_slope = factor_t > 0
						 ? static_cast<double>(num) / static_cast<double>(factor_t)
						 : 1.0;
	}
	double const denom = std::sqrt(double(factor_t)) * std::sqrt(double(factor_i));
	if (denom < 1e-6)
		return false;
	out = double(num) / denom;
	return true;
}

BestNccResult FindBestNccScalar(GrayView templ, GrayView image,
								int min_ox, int min_oy, int max_ox, int max_oy, NccTieBreak tie) {
	NccBestState state;
	state.tie = tie;
	for (int oy = min_oy; oy <= max_oy; ++oy) {
		for (int ox = min_ox; ox <= max_ox; ++ox) {
			double ncc = 0.0;
			if (!ZeroMeanNccScalar(templ, image, ox, oy, ncc))
				continue;
			state.Consider(ncc, ox, oy);
		}
	}
	return state.best;
}

double ParabolicSubpixel(double left, double center, double right) {
	// A parabola only models a peak through a local maximum. A candidate
	// clamped at the search-window edge has its higher neighbour outside
	// the window; through such a triple the parabola opens upward and its
	// vertex sits on the side opposite the true peak -- P(1.0, 0.8, 0.79)
	// = +0.553 steers half a pixel away from the higher left neighbour --
	// and the clamp cannot undo it. Hold the integer peak instead of
	// steering away from the true one.
	if (center < left || center < right)
		return 0.0;
	double const denom = left - 2.0 * center + right;
	if (denom == 0.0)
		return 0.0;
	double const delta = 0.5 * (left - right) / denom;
	return std::clamp(delta, -0.75, 0.75);
}

namespace {

double MedianOf(std::vector<double> values) {
	if (values.empty())
		return 0.0;
	auto const mid = values.begin() + (values.size() - 1) / 2;
	std::nth_element(values.begin(), mid, values.end());
	return *mid;
}

} // namespace

// Occlusion-robust block selection; see ncc.h for the provenance and the
// exact rejection rule.
bool SelectInlierBlocks(std::vector<double> const& block_residuals,
						RobustInlierParams const& params,
						std::vector<bool>& kept) {
	std::size_t const n = block_residuals.size();
	if (n < params.min_blocks)
		return false;

	std::size_t const min_kept = std::max<std::size_t>(
		params.min_blocks,
		static_cast<std::size_t>(std::ceil(
			double(n) * (1.0 - params.max_reject_fraction))));

	kept.assign(n, true);
	std::size_t kept_count = n;
	for (int round = 0; round < params.max_rounds; ++round) {
		std::vector<double> current;
		current.reserve(kept_count);
		for (std::size_t i = 0; i < n; ++i)
			if (kept[i])
				current.push_back(block_residuals[i]);

		double const median = MedianOf(current);
		std::vector<double> abs_dev;
		abs_dev.reserve(current.size());
		for (double v : current)
			abs_dev.push_back(std::abs(v - median));
		// Same floor as SelectInlierPixels: a MAD of 0 says the blocks at
		// the median share one value, and a cutoff pinned at the median
		// would cull every block only half a gray level above it.
		double const mad = std::max(MedianOf(abs_dev), 1.0);
		double const cutoff = median + params.outlier_k * 1.4826 * mad;

		std::size_t next_count = 0;
		for (std::size_t i = 0; i < n; ++i) {
			if (!kept[i])
				continue;
			if (block_residuals[i] <= cutoff)
				++next_count;
		}
		if (next_count == kept_count)
			break; // converged; nothing new to trim
		if (next_count < min_kept)
			return false;
		for (std::size_t i = 0; i < n; ++i)
			if (kept[i] && block_residuals[i] > cutoff)
				kept[i] = false;
		kept_count = next_count;
	}
	return true;
}

// Pixel-level inlier selection; see ncc.h for why the gate is a single
// robust noise estimate rather than the iterative block trim.
bool SelectInlierPixels(std::vector<double> const& pixel_diffs,
						RobustInlierParams const& params,
						std::vector<bool>& kept) {
	std::size_t const n = pixel_diffs.size();
	if (n < params.min_blocks)
		return false;

	// The gate anchors on the low side of the distribution: the lower
	// quartile for the level and the lower half for the scale. With any
	// occlusion within the reject cap the anchor stays on visible pixels,
	// while a plain median+MAD flips once half the template is occluded and
	// would then size the gate to the occluder itself.
	std::vector<double> sorted(pixel_diffs);
	std::sort(sorted.begin(), sorted.end());
	double const anchor = sorted[(n - 1) / 4];
	std::vector<double> lower_dev;
	lower_dev.reserve(n / 2);
	for (std::size_t i = 0; i < n / 2; ++i)
		lower_dev.push_back(std::abs(sorted[i] - anchor));
	// A MAD of 0 only says the pixels near the lower quartile share one
	// value, not that the noise is zero. Without a floor the cutoff
	// collapses onto the anchor itself, and then a visible side whose
	// pixels stray a single gray level (compression noise alone does
	// that) fails the selection and the whole occlusion rescue with it.
	// One gray level is the smallest spread a real match carries; the
	// 60-level occluder this gate exists for stays far above it.
	double const scale = std::max(MedianOf(lower_dev), 1.0);
	double const cutoff = anchor + params.outlier_k * 1.4826 * scale;

	std::size_t kept_count = 0;
	for (double v : pixel_diffs)
		if (v <= cutoff)
			++kept_count;
	std::size_t const min_kept = std::max<std::size_t>(
		params.min_blocks,
		static_cast<std::size_t>(std::ceil(
			double(n) * (1.0 - params.max_reject_fraction))));
	if (kept_count < min_kept)
		return false;

	kept.clear();
	kept.reserve(n);
	for (double v : pixel_diffs)
		kept.push_back(v <= cutoff);
	return true;
}

BestNccResult FindBestNcc(GrayView templ, GrayView image,
						  int min_ox, int min_oy, int max_ox, int max_oy, NccTieBreak tie) {
#ifdef AEGISUB_WITH_HIGHWAY
	return simd::FindBestNccSimd(templ, image, min_ox, min_oy, max_ox, max_oy,
								 tie);
#else
	return FindBestNccScalar(templ, image, min_ox, min_oy, max_ox, max_oy, tie);
#endif
}

namespace {

// NCC at one candidate offset, or a score below any real correlation when
// the window is flat/invalid (never a competitor).
double NccAt(GrayView templ, GrayView image, int ox, int oy) {
	double ncc = 0.0;
	if (!ZeroMeanNccScalar(templ, image, ox, oy, ncc))
		return -2.0;
	return ncc;
}

bool IsDisplacedLocalMax(GrayView templ, GrayView image,
                         int min_ox, int min_oy, int max_ox, int max_oy,
                         int x, int y, double score) {
	// Neighbours outside the scan rectangle do not block: the scan window is
	// the tracker's actual search extent, not a periodic grid. Strictly
	// greater blocks: a plateau (periodic texture flat along one axis) still
	// counts as a peak, while a monotone shoulder always has a better
	// neighbour toward the true maximum. The +-2 window matches the width of
	// a correlation lobe's bumps, so a smooth template's shoulder ripples do
	// not register as competing peaks.
	constexpr int kRadius = 2;
	for (int dy = -kRadius; dy <= kRadius; ++dy)
		for (int dx = -kRadius; dx <= kRadius; ++dx) {
			if (dx == 0 && dy == 0) continue;
			int const nx = x + dx;
			int const ny = y + dy;
			if (nx < min_ox || nx > max_ox || ny < min_oy || ny > max_oy)
				continue;
			if (NccAt(templ, image, nx, ny) > score)
				return false;
		}
	return true;
}

} // namespace

bool HasCompetingNccPeak(GrayView templ, GrayView image,
	int min_ox, int min_oy, int max_ox, int max_oy,
	int around_x, int around_y, int excl_x, int excl_y,
	double ratio, double min_score) {
	if (!templ.data || !image.data || templ.width <= 0 || templ.height <= 0
	    || templ.stride < templ.width || image.stride < image.width)
		return false;
	double const threshold = ratio * min_score;

	for (int oy = min_oy; oy <= max_oy; ++oy) {
		for (int ox = min_ox; ox <= max_ox; ++ox) {
			if (std::abs(ox - around_x) <= excl_x
			    && std::abs(oy - around_y) <= excl_y)
				continue;
			double const ncc = NccAt(templ, image, ox, oy);
			if (ncc < threshold)
				continue;
			if (IsDisplacedLocalMax(templ, image, min_ox, min_oy, max_ox,
			                        max_oy, ox, oy, ncc))
				return true;
		}
	}
	return false;
}

} // namespace aegisub::motion_track
