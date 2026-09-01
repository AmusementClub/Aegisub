#include "similarity_backend.h"

#include "ncc.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace aegisub::motion_track {

namespace {

std::uint64_t HashBytes(std::uint8_t const* data, size_t size) {
	std::uint64_t hash = 14695981039346656037ull; // FNV-1a 64-bit offset basis
	for (size_t i = 0; i < size; ++i) {
		hash ^= data[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

bool ValidView(GrayView const& view) {
	return view.data != nullptr && view.width > 0 && view.height > 0
	    && view.stride >= view.width;
}

// Bilinear sample of a float field; false when (x, y) leaves the inner
// [1, size-2] margin so that gradients exist on all sides.
bool SampleField(float const* field, int field_width, int field_height,
                 double x, double y, float& out) {
	if (x < 1.0 || y < 1.0 || x > field_width - 2.0 || y > field_height - 2.0)
		return false;
	int const x0 = int(x);
	int const y0 = int(y);
	double const fx = x - x0;
	double const fy = y - y0;
	auto const at = [&](int xx, int yy) {
		return double(field[size_t(yy) * field_width + size_t(xx)]);
	};
	out = float((1.0 - fy)
	            * ((1.0 - fx) * at(x0, y0) + fx * at(x0 + 1, y0))
	        + fy * ((1.0 - fx) * at(x0, y0 + 1) + fx * at(x0 + 1, y0 + 1)));
	return true;
}

// Bilinear sample of the gray view itself, same inner-margin rule.
bool SampleGray(GrayView view, double x, double y, float& out) {
	if (x < 1.0 || y < 1.0 || x > view.width - 2.0 || y > view.height - 2.0)
		return false;
	int const x0 = int(x);
	int const y0 = int(y);
	double const fx = x - x0;
	double const fy = y - y0;
	auto const at = [&](int xx, int yy) {
		return double(view.data[int64_t(yy) * view.stride + xx]);
	};
	out = float((1.0 - fy)
	            * ((1.0 - fx) * at(x0, y0) + fx * at(x0 + 1, y0))
	        + fy * ((1.0 - fx) * at(x0, y0 + 1) + fx * at(x0 + 1, y0 + 1)));
	return true;
}

// Solves A*x = b (4x4, row major) with partial pivoting. Returns false when
// the system is numerically singular.
bool Solve4(double a[4][4], double b[4], double x[4]) {
	for (int col = 0; col < 4; ++col) {
		int pivot = col;
		for (int row = col + 1; row < 4; ++row)
			if (std::abs(a[row][col]) > std::abs(a[pivot][col]))
				pivot = row;
		if (std::abs(a[pivot][col]) < 1e-12)
			return false;
		if (pivot != col) {
			std::swap(a[pivot], a[col]);
			std::swap(b[pivot], b[col]);
		}
		for (int row = col + 1; row < 4; ++row) {
			double const f = a[row][col] / a[col][col];
			for (int k = col; k < 4; ++k)
				a[row][k] -= f * a[col][k];
			b[row] -= f * b[col];
		}
	}
	for (int i = 3; i >= 0; --i) {
		double s = b[i];
		for (int k = i + 1; k < 4; ++k)
			s -= a[i][k] * x[k];
		x[i] = s / a[i][i];
	}
	return true;
}

} // namespace

SimilarityTrackerBackend::SimilarityTrackerBackend(
	SimilarityTrackerConfig config)
	: config_(config) {}

TrackModel SimilarityTrackerBackend::Model() const {
	return TrackModel::Similarity;
}

std::string_view SimilarityTrackerBackend::Name() const noexcept {
	return "ecc-similarity";
}

void SimilarityTrackerBackend::ClearTemplate() {
	has_template_ = false;
	roi_w_ = 0;
	roi_h_ = 0;
	template_pixels_.clear();
	template_pixels_.shrink_to_fit();
}

TrackStatus SimilarityTrackerBackend::Reset(TrackerSeed const& seed) {
	ClearTemplate();

	if (seed.model != TrackModel::Similarity)
		return TrackStatus::Unsupported;
	if (!ValidView(seed.template_gray))
		return TrackStatus::InvalidInput;

	roi_w_ = seed.template_gray.width;
	roi_h_ = seed.template_gray.height;
	template_pixels_.resize(size_t(roi_w_) * size_t(roi_h_));
	for (int y = 0; y < roi_h_; ++y) {
		std::copy_n(
			seed.template_gray.data + int64_t(y) * seed.template_gray.stride,
			roi_w_,
			template_pixels_.begin() + int64_t(y) * roi_w_);
	}
	has_template_ = true;
	return TrackStatus::Ok;
}

TrackStepResult SimilarityTrackerBackend::Step(TrackStepRequest const& request) {
	TrackStepResult result;
	result.frame = request.frame;

	if (!has_template_ || !ValidView(request.image)) {
		result.status = TrackStatus::InvalidInput;
		return result;
	}
	GrayView const image = request.image;

	auto const expected_left_x = static_cast<int>(std::floor(
		request.search_center_x - (roi_w_ - 1) / 2.0 + 0.5));
	auto const expected_left_y = static_cast<int>(std::floor(
		request.search_center_y - (roi_h_ - 1) / 2.0 + 0.5));
	int const anchor_x = expected_left_x - request.image_origin_x;
	int const anchor_y = expected_left_y - request.image_origin_y;
	if (anchor_x < 0 || anchor_y < 0 || anchor_x + roi_w_ > image.width
	    || anchor_y + roi_h_ > image.height) {
		result.status = TrackStatus::Failed;
		result.failure = TrackFailureReason::Offscreen;
		return result;
	}

	double rotation = request.init_rotation;
	double scale = request.init_scale > 0.0 ? request.init_scale : 1.0;
	double const init_rotation = rotation;
	double const init_scale = scale;
	double tx = 0.0;
	double ty = 0.0;

	// Image gradients (central differences, clamped borders) on the crop,
	// row-packed float planes.
	std::vector<float> grad_x(size_t(image.width) * size_t(image.height));
	std::vector<float> grad_y(size_t(image.width) * size_t(image.height));
	for (int y = 0; y < image.height; ++y) {
		int const yn = std::min(y + 1, image.height - 1);
		int const yp = std::max(y - 1, 0);
		for (int x = 0; x < image.width; ++x) {
			int const xn = std::min(x + 1, image.width - 1);
			int const xp = std::max(x - 1, 0);
			size_t const i = size_t(y) * image.width + size_t(x);
			grad_x[i] = 0.5f * (image.data[int64_t(y) * image.stride + xn]
			                    - image.data[int64_t(y) * image.stride + xp]);
			grad_y[i] = 0.5f * (image.data[int64_t(yn) * image.stride + x]
			                    - image.data[int64_t(yp) * image.stride + x]);
		}
	}

	// Template sample set, deterministically subsampled when large.
	int const sample_stride = std::max(
	    1, int(std::ceil(std::sqrt(
	           double(roi_w_) * roi_h_ / std::max(1, config_.max_samples)))));
	double const cx = anchor_x + (roi_w_ - 1) / 2.0;
	double const cy = anchor_y + (roi_h_ - 1) / 2.0;

	std::vector<float> t_vals;
	std::vector<double> xs, ys;
	for (int y = 0; y < roi_h_; y += sample_stride)
		for (int x = 0; x < roi_w_; x += sample_stride) {
			t_vals.push_back(
			    float(template_pixels_[size_t(y) * roi_w_ + size_t(x)]));
			xs.push_back(x - (roi_w_ - 1) / 2.0);
			ys.push_back(y - (roi_h_ - 1) / 2.0);
		}
	size_t const n = t_vals.size();

	// A flat template carries no alignment information; the refinement below
	// would wander off the crop. Same verdict as the translation backend's
	// zero-denominator path.
	{
		double mean_t = 0.0;
		for (float v : t_vals) mean_t += v;
		mean_t /= double(n);
		double var_t = 0.0;
		for (float v : t_vals) var_t += (v - mean_t) * (v - mean_t);
		var_t /= double(n);
		if (var_t < 1.0) {
			result.status = TrackStatus::Failed;
			result.failure = TrackFailureReason::NccLow;
			return result;
		}
	}

	auto warped_pos = [&](size_t i, double cos_r, double sin_r,
	                      double& px, double& py) {
		px = cx + tx + scale * (cos_r * xs[i] - sin_r * ys[i]);
		py = cy + ty + scale * (sin_r * xs[i] + cos_r * ys[i]);
	};

	// Forward-additive Gauss-Newton with trust-region clamps. Empty weights
	// mean unit weights; otherwise weights[i] scales sample i's contribution
	// (IRLS second pass). Sets offscreen when the warp leaves the crop.
	bool offscreen = false;
	auto run_iterations = [&](std::vector<double> const& weights) {
		double prev_error = std::numeric_limits<double>::infinity();
		int worsening = 0;
		for (int iter = 0; iter < config_.max_iterations; ++iter) {
			double const cos_r = std::cos(rotation);
			double const sin_r = std::sin(rotation);

			double h[4][4] = {};
			double b[4] = {};
			double error_sum = 0.0;
			size_t valid = 0;
			for (size_t i = 0; i < n; ++i) {
				double px = 0.0, py = 0.0;
				warped_pos(i, cos_r, sin_r, px, py);
				float iv = 0.f, gx = 0.f, gy = 0.f;
				if (!SampleGray(image, px, py, iv))
					continue;
				if (!SampleField(grad_x.data(), image.width, image.height,
				                 px, py, gx))
					continue;
				if (!SampleField(grad_y.data(), image.width, image.height,
				                 px, py, gy))
					continue;
				++valid;

				double const w = weights.empty() ? 1.0 : weights[i];
				if (w <= 0.0)
					continue;
				double const e = double(iv) - t_vals[i];
				double const dpx_dth = scale * (-sin_r * xs[i] - cos_r * ys[i]);
				double const dpy_dth = scale * (cos_r * xs[i] - sin_r * ys[i]);
				double const dpx_ds = cos_r * xs[i] - sin_r * ys[i];
				double const dpy_ds = sin_r * xs[i] + cos_r * ys[i];
				double const j[4] = {
					gx, gy,
					gx * dpx_dth + gy * dpy_dth,
					gx * dpx_ds + gy * dpy_ds
				};
				for (int r = 0; r < 4; ++r) {
					b[r] += w * j[r] * e;
					for (int c = 0; c < 4; ++c)
						h[r][c] += w * j[r] * j[c];
				}
				error_sum += w * e * e;
			}
			if (valid * 2 < n) {
				// More than half the template left the usable crop area.
				offscreen = true;
				return;
			}

			// Tikhonov damping keeps flat-texture Hessians solvable.
			double const trace = h[0][0] + h[1][1] + h[2][2] + h[3][3];
			double const damp = 1e-9 * std::max(1.0, trace);
			for (int r = 0; r < 4; ++r) h[r][r] += damp;

			double delta[4];
			if (!Solve4(h, b, delta))
				return;
			// Gauss-Newton update: p -= (J^T J)^-1 J^T e.
			for (int r = 0; r < 4; ++r)
				delta[r] = -delta[r];

			tx += std::clamp(delta[0], -config_.max_iteration_translation,
			                 config_.max_iteration_translation);
			ty += std::clamp(delta[1], -config_.max_iteration_translation,
			                 config_.max_iteration_translation);
			rotation += std::clamp(delta[2], -config_.max_iteration_rotation,
			                       config_.max_iteration_rotation);
			scale *= std::exp(std::clamp(delta[3] / scale,
			                             -config_.max_iteration_log_scale,
			                             config_.max_iteration_log_scale));
			if (scale < 0.1 || scale > 10.0)
				return;

			if (error_sum > prev_error * 1.5 + 1e-9) {
				if (++worsening >= 3)
					return;
			} else {
				worsening = 0;
			}
			prev_error = error_sum;

			double const max_delta = std::max(
			    {std::abs(delta[0]), std::abs(delta[1]), std::abs(delta[2]),
			     std::abs(delta[3])});
			if (max_delta < config_.step_tolerance)
				return;
		}
	};

	run_iterations({});
	if (offscreen) {
		result.status = TrackStatus::Failed;
		result.failure = TrackFailureReason::Offscreen;
		return result;
	}

	// IRLS reweighting: samples whose residual survives a Tukey biweight on
	// the robust sigma (template edge riding on background after rotation,
	// partial occlusion) get ~0 weight and the pose is refined again. Two
	// passes let the weights follow the improving fit.
	for (int irls_pass = 0; irls_pass < 2; ++irls_pass) {
		double const cos_r = std::cos(rotation);
		double const sin_r = std::sin(rotation);
		std::vector<double> residuals(n, 0.0);
		std::vector<bool> usable(n, false);
		std::vector<double> abs_r;
		for (size_t i = 0; i < n; ++i) {
			double px = 0.0, py = 0.0;
			warped_pos(i, cos_r, sin_r, px, py);
			float iv = 0.f;
			if (!SampleGray(image, px, py, iv))
				continue;
			residuals[i] = double(iv) - t_vals[i];
			usable[i] = true;
			abs_r.push_back(std::abs(residuals[i]));
		}
		if (abs_r.size() * 2 < n || abs_r.empty())
			break;
		auto const mid = abs_r.begin() + (abs_r.size() - 1) / 2;
		std::nth_element(abs_r.begin(), mid, abs_r.end());
		double const sigma = 1.4826 * (*mid);
		if (sigma <= 0.25)
			break;
		double const c = 4.685 * sigma;
		std::vector<double> weights(n, 0.0);
		double weight_sum = 0.0;
		for (size_t i = 0; i < n; ++i) {
			if (!usable[i]) continue;
			double const r = residuals[i] / c;
			weights[i] = r * r < 1.0
			    ? (1.0 - r * r) * (1.0 - r * r)
			    : 0.0;
			weight_sum += weights[i];
		}
		if (weight_sum * 4 < double(n))
			break; // almost everything is an outlier; keep the plain fit
		run_iterations(weights);
		if (offscreen) {
			result.status = TrackStatus::Failed;
			result.failure = TrackFailureReason::Offscreen;
			return result;
		}
	}

	// Per-step sanity gates: the refinement must not have run away from the
	// pose it was initialized with.
	if (std::abs(rotation - init_rotation) > config_.max_rotation_per_step
	    || scale / init_scale < config_.min_scale_ratio_per_step
	    || scale / init_scale > config_.max_scale_ratio_per_step) {
		result.status = TrackStatus::Failed;
		result.failure = TrackFailureReason::JumpTooLarge;
		return result;
	}

	// Final quality metrics: zero-mean NCC and MAD residual between the
	// template and the warped image over the valid samples. Per-sample data
	// is retained so a failed plain score can be rescored on inlier blocks
	// only (partial occlusion); the plain path is byte-identical to the
	// historical one for frames that pass it.
	double const cos_r = std::cos(rotation);
	double const sin_r = std::sin(rotation);
	std::vector<bool> usable(n, false);
	std::vector<double> sample_t(n, 0.0), sample_v(n, 0.0);
	size_t counted = 0;
	for (size_t i = 0; i < n; ++i) {
		double px = 0.0, py = 0.0;
		warped_pos(i, cos_r, sin_r, px, py);
		float iv = 0.f;
		if (!SampleGray(image, px, py, iv))
			continue;
		sample_t[i] = double(t_vals[i]);
		sample_v[i] = double(iv);
		usable[i] = true;
		++counted;
	}
	if (counted * 2 < n) {
		result.status = TrackStatus::Failed;
		result.failure = TrackFailureReason::Offscreen;
		return result;
	}

	// Zero-mean NCC plus MAD residual over whichever samples `include`
	// admits. Returns false when the kept subset is degenerate.
	auto metric_over = [&](auto&& include, double& out_ncc,
						   double& out_median) {
		double st = 0, si = 0, st2 = 0, si2 = 0, sti = 0;
		std::vector<int> dd;
		size_t c = 0;
		for (size_t i = 0; i < n; ++i) {
			if (!usable[i] || !include(i))
				continue;
			double const t = sample_t[i];
			double const v = sample_v[i];
			st += t;
			si += v;
			st2 += t * t;
			si2 += v * v;
			sti += t * v;
			dd.push_back(int(std::lround(std::abs(t - v))));
			++c;
		}
		if (c == 0)
			return false;
		double const nn = double(c);
		double const num = nn * sti - st * si;
		double const den_t = nn * st2 - st * st;
		double const den_i = nn * si2 - si * si;
		// A flat side makes the correlation ratio 0/0 in exact arithmetic and
		// a random value after float cancellation; reject it on the variance
		// instead of the ratio.
		if (den_t < 1.0 || den_i < 1.0)
			return false;
		double const denom = std::sqrt(den_t) * std::sqrt(den_i);
		if (denom < 1e-6)
			return false;
		out_ncc = num / denom;
		auto const mid = dd.begin() + (dd.size() - 1) / 2;
		std::nth_element(dd.begin(), mid, dd.end());
		out_median = double(*mid);
		return true;
	};

	double ncc = 0.0;
	double median_diff = 0.0;
	if (!metric_over([](size_t) { return true; }, ncc, median_diff)) {
		result.status = TrackStatus::Failed;
		result.failure = TrackFailureReason::NccLow;
		return result;
	}
	if (ncc < config_.ncc_min && config_.robust_inlier_scoring) {
		// Occlusion fallback on the acceptance score: bin the samples by
		// their position in the template into 8x8 blocks, reject blocks
		// whose residual is a robust outlier, rescore on the rest.
		// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
		// (ISC license).
		int const blocks_x = (roi_w_ + 7) / 8;
		int const blocks_y = (roi_h_ + 7) / 8;
		std::vector<size_t> block_of(n, 0);
		for (size_t i = 0; i < n; ++i) {
			int const tx = int(std::lround(xs[i] + (roi_w_ - 1) / 2.0));
			int const ty = int(std::lround(ys[i] + (roi_h_ - 1) / 2.0));
			int const bx = std::clamp(tx / 8, 0, blocks_x - 1);
			int const by = std::clamp(ty / 8, 0, blocks_y - 1);
			block_of[i] = size_t(by) * blocks_x + bx;
		}
		size_t const block_count = size_t(blocks_x) * blocks_y;
		std::vector<double> sums(block_count, 0.0);
		std::vector<size_t> counts(block_count, 0);
		for (size_t i = 0; i < n; ++i) {
			if (!usable[i])
				continue;
			sums[block_of[i]] += std::abs(sample_t[i] - sample_v[i]);
			++counts[block_of[i]];
		}
		std::vector<double> block_residual;
		std::vector<size_t> block_first_sample;
		for (size_t b = 0; b < block_count; ++b) {
			if (counts[b] == 0)
				continue;
			block_residual.push_back(sums[b] / counts[b]);
			block_first_sample.push_back(b);
		}
		RobustInlierParams params;
		params.max_reject_fraction = config_.max_occlusion_fraction;
		std::vector<bool> kept_blocks;
		if (SelectInlierBlocks(block_residual, params, kept_blocks)) {
			std::vector<bool> kept_by_block(block_count, false);
			for (size_t k = 0; k < block_first_sample.size(); ++k)
				if (kept_blocks[k])
					kept_by_block[block_first_sample[k]] = true;
			double rescored_ncc = 0.0;
			double rescored_median = 0.0;
			if (metric_over([&](size_t i) { return kept_by_block[block_of[i]]; },
							rescored_ncc, rescored_median)) {
				ncc = rescored_ncc;
				median_diff = rescored_median;
			}
		}
	}
	if (ncc < config_.ncc_min) {
		result.status = TrackStatus::Failed;
		result.failure = TrackFailureReason::NccLow;
		return result;
	}
	if (median_diff > config_.residual_max) {
		result.status = TrackStatus::Failed;
		result.failure = TrackFailureReason::ResidualHigh;
		return result;
	}

	result.status = TrackStatus::Ok;
	result.confidence = ncc;
	result.residual = median_diff;
	result.transform.matrix[0] = scale * cos_r;
	result.transform.matrix[1] = -scale * sin_r;
	result.transform.matrix[3] = scale * sin_r;
	result.transform.matrix[4] = scale * cos_r;
	result.candidate_center_x = request.image_origin_x + cx + tx;
	result.candidate_center_y = request.image_origin_y + cy + ty;
	return result;
}

std::uint64_t SimilarityTrackerBackend::TemplateHash() const noexcept {
	if (!has_template_ || template_pixels_.empty())
		return 0;
	return HashBytes(template_pixels_.data(), template_pixels_.size());
}

} // namespace aegisub::motion_track
