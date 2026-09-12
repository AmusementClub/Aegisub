#include "planar_backend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace aegisub::motion_track {
namespace {

using Matrix = std::array<double, 9>;

bool ValidView(GrayView view) {
	return view.data && view.width >= 4 && view.height >= 4 && view.stride >= view.width;
}

bool Sample(GrayView image, double x, double y, double& value,
			double& gx, double& gy) {
	if (!std::isfinite(x) || !std::isfinite(y) || x < 1 || y < 1 ||
		x >= image.width - 2 || y >= image.height - 2)
		return false;
	int const ix = static_cast<int>(x), iy = static_cast<int>(y);
	double const fx = x - ix, fy = y - iy;
	auto pixel = [&](int px, int py) {
		return static_cast<double>(image.data[static_cast<std::int64_t>(py) * image.stride + px]);
	};
	auto interpolate = [&](int dx, int dy) {
		return (1 - fy) * ((1 - fx) * pixel(ix + dx, iy + dy) + fx * pixel(ix + dx + 1, iy + dy)) +
			   fy * ((1 - fx) * pixel(ix + dx, iy + dy + 1) + fx * pixel(ix + dx + 1, iy + dy + 1));
	};
	value = interpolate(0, 0);
	gx = (interpolate(1, 0) - interpolate(-1, 0)) * 0.5;
	gy = (interpolate(0, 1) - interpolate(0, -1)) * 0.5;
	return true;
}

bool Solve(double a[8][8], double b[8], double x[8], int count) {
	for (int col = 0; col < count; ++col) {
		int pivot = col;
		for (int row = col + 1; row < count; ++row)
			if (std::abs(a[row][col]) > std::abs(a[pivot][col]))
				pivot = row;
		if (std::abs(a[pivot][col]) < 1e-10)
			return false;
		if (pivot != col) {
			std::swap(a[pivot], a[col]);
			std::swap(b[pivot], b[col]);
		}
		for (int row = col + 1; row < count; ++row) {
			double const factor = a[row][col] / a[col][col];
			for (int k = col; k < count; ++k)
				a[row][k] -= factor * a[col][k];
			b[row] -= factor * b[col];
		}
	}
	for (int row = count - 1; row >= 0; --row) {
		double value = b[row];
		for (int k = row + 1; k < count; ++k)
			value -= a[row][k] * x[k];
		x[row] = value / a[row][row];
	}
	return true;
}

double Median(std::vector<double> values) {
	if (values.empty())
		return std::numeric_limits<double>::infinity();
	auto middle = values.begin() + (values.size() - 1) / 2;
	std::nth_element(values.begin(), middle, values.end());
	return *middle;
}

bool Project(Matrix const& h, double x, double y, double& px, double& py) {
	double const w = h[6] * x + h[7] * y + 1;
	if (!std::isfinite(w) || w < 0.2)
		return false;
	px = (h[0] * x + h[1] * y + h[2]) / w;
	py = (h[3] * x + h[4] * y + h[5]) / w;
	return std::isfinite(px) && std::isfinite(py);
}

double Determinant(Matrix const& h) {
	return h[0] * (h[4] - h[5] * h[7]) - h[1] * (h[3] - h[5] * h[6]) +
		   h[2] * (h[3] * h[7] - h[4] * h[6]);
}

struct Point {
	double x, y, value;
};

} // namespace

PlanarTrackerBackend::PlanarTrackerBackend(TrackModel model, PlanarTrackerConfig config)
	: model_(model), config_(config) {}

TrackModel PlanarTrackerBackend::Model() const { return model_; }

std::string_view PlanarTrackerBackend::Name() const noexcept {
	return model_ == TrackModel::Affine ? "ecc-affine" : "ecc-homography";
}

TrackStatus PlanarTrackerBackend::Reset(TrackerSeed const& seed) {
	template_pixels_.clear();
	width_ = height_ = 0;
	if ((model_ != TrackModel::Affine && model_ != TrackModel::Homography) || seed.model != model_)
		return TrackStatus::Unsupported;
	if (!ValidView(seed.template_gray))
		return TrackStatus::InvalidInput;
	width_ = seed.template_gray.width;
	height_ = seed.template_gray.height;
	template_pixels_.resize(static_cast<size_t>(width_) * height_);
	for (int y = 0; y < height_; ++y)
		std::copy_n(seed.template_gray.data + static_cast<std::int64_t>(y) * seed.template_gray.stride,
					width_, template_pixels_.data() + static_cast<size_t>(y) * width_);
	return TrackStatus::Ok;
}

TrackStepResult PlanarTrackerBackend::Step(TrackStepRequest const& request) {
	TrackStepResult result;
	result.frame = request.frame;
	if (template_pixels_.empty() || !ValidView(request.image) ||
		!std::isfinite(request.search_center_x) || !std::isfinite(request.search_center_y)) {
		result.status = TrackStatus::InvalidInput;
		return result;
	}
	auto fail = [&](TrackFailureReason reason) {
		result.status = TrackStatus::Failed;
		result.failure = reason;
		return result;
	};
	double const radius = std::max(width_, height_) * 0.5;
	double const cx = request.search_center_x - request.image_origin_x;
	double const cy = request.search_center_y - request.image_origin_y;
	Matrix h = request.init_transform.matrix;
	if (!std::ranges::all_of(h, [](double v) { return std::isfinite(v); }) ||
		std::abs(h[8] - 1) > 1e-6)
		return fail(TrackFailureReason::JumpTooLarge);
	h[2] = h[5] = 0;
	h[6] *= radius;
	h[7] *= radius;
	if (model_ == TrackModel::Affine)
		h[6] = h[7] = 0;
	Matrix const initial = h;
	if (Determinant(h) <= 1e-6)
		return fail(TrackFailureReason::JumpTooLarge);
	int const parameter_count = model_ == TrackModel::Affine ? 6 : 8;
	int const stride = std::max(1, static_cast<int>(std::ceil(std::sqrt(
									   static_cast<double>(width_) * height_ / std::max(64, config_.max_samples)))));
	std::vector<Point> points;
	for (int y = 0; y < height_; y += stride)
		for (int x = 0; x < width_; x += stride)
			points.push_back({.x = (x - (width_ - 1) * 0.5) / radius,
							  .y = (y - (height_ - 1) * 0.5) / radius,
							  .value = static_cast<double>(template_pixels_[static_cast<size_t>(y) * width_ + x])});
	double mean = 0, variance = 0;
	for (auto const& point : points)
		mean += point.value;
	mean /= static_cast<double>(points.size());
	for (auto const& point : points)
		variance += (point.value - mean) * (point.value - mean);
	if (variance < static_cast<double>(points.size()))
		return fail(TrackFailureReason::NccLow);

	std::vector<double> weights(points.size(), 1);
	std::vector<double> residuals(points.size());
	std::vector<bool> usable(points.size());
	auto reweight = [&] {
		std::vector<double> absolute;
		for (size_t i = 0; i < points.size(); ++i) {
			auto const& point = points[i];
			double px = 0, py = 0, value = 0, gx = 0, gy = 0;
			usable[i] = Project(h, point.x, point.y, px, py) &&
						Sample(request.image, cx + radius * px, cy + radius * py, value, gx, gy);
			if (usable[i]) {
				residuals[i] = value - point.value;
				absolute.push_back(std::abs(residuals[i]));
			}
		}
		double const cutoff = std::max(4.0, 4.685 * 1.4826 * Median(absolute));
		for (size_t i = 0; i < points.size(); ++i) {
			double const r = residuals[i] / cutoff;
			weights[i] = usable[i] && std::abs(r) < 1 ? (1 - r * r) * (1 - r * r) : 0;
		}
	};
	reweight();
	// Weight the prior before refining so an occluder cannot first bend the
	// plane toward itself. Eight bounded IRLS fits let the inlier set settle
	// after the initial displacement, including occlusion boundary pixels.
	for (int pass = 0; pass < 8; ++pass) {
		for (int iteration = 0; iteration < config_.max_iterations; ++iteration) {
			double normal[8][8] = {}, rhs[8] = {}, delta[8] = {};
			double error = 0;
			size_t valid = 0;
			for (size_t i = 0; i < points.size(); ++i) {
				auto const& point = points[i];
				double px = 0, py = 0, value = 0, gx = 0, gy = 0;
				if (!Project(h, point.x, point.y, px, py) ||
					!Sample(request.image, cx + radius * px, cy + radius * py, value, gx, gy))
					continue;
				++valid;
				double const e = value - point.value;
				double const inv_w = radius / (h[6] * point.x + h[7] * point.y + 1);
				gx *= inv_w;
				gy *= inv_w;
				double const perspective = -(gx * px + gy * py);
				double const jacobian[8] = {gx * point.x, gx * point.y, gx,
											gy * point.x, gy * point.y, gy, perspective * point.x, perspective * point.y};
				for (int row = 0; row < parameter_count; ++row) {
					rhs[row] += weights[i] * jacobian[row] * e;
					for (int col = 0; col < parameter_count; ++col)
						normal[row][col] += weights[i] * jacobian[row] * jacobian[col];
				}
				error += weights[i] * e * e;
			}
			if (valid * 4 < points.size() * 3)
				return fail(TrackFailureReason::Offscreen);
			// A rank-deficient texture cannot constrain all requested DOFs.
			if (!Solve(normal, rhs, delta, parameter_count))
				return fail(TrackFailureReason::NccLow);
			double largest = 0;
			for (int k = 0; k < parameter_count; ++k)
				largest = std::max(largest, std::abs(delta[k]));
			double const trust = largest > 0.15 ? 0.15 / largest : 1;
			bool accepted = false;
			for (int attempt = 0; attempt < 6; ++attempt) {
				double const factor = std::ldexp(trust, -attempt);
				Matrix candidate = h;
				for (int k = 0; k < parameter_count; ++k)
					candidate[k] -= factor * delta[k];
				if (Determinant(candidate) <= 1e-6)
					continue;
				double next_error = 0;
				size_t next_valid = 0;
				for (size_t i = 0; i < points.size(); ++i) {
					auto const& point = points[i];
					double px = 0, py = 0, value = 0, gx = 0, gy = 0;
					if (!Project(candidate, point.x, point.y, px, py) ||
						!Sample(request.image, cx + radius * px, cy + radius * py, value, gx, gy))
						continue;
					++next_valid;
					double const e = value - point.value;
					next_error += weights[i] * e * e;
				}
				if (next_valid >= valid && next_error <= error + 1e-6) {
					h = candidate;
					accepted = true;
					break;
				}
			}
			if (!accepted || largest * trust < 1e-6)
				break;
		}
		reweight();
	}

	double const area_ratio = Determinant(h) / Determinant(initial);
	if (area_ratio < config_.min_area_ratio_per_step || area_ratio > config_.max_area_ratio_per_step)
		return fail(TrackFailureReason::JumpTooLarge);
	for (double y : {-0.5 * height_ / radius, 0.5 * height_ / radius})
		for (double x : {-0.5 * width_ / radius, 0.5 * width_ / radius}) {
			double px = 0, py = 0, ix = 0, iy = 0;
			if (!Project(h, x, y, px, py) || !Project(initial, x, y, ix, iy) ||
				radius * std::hypot(px - ix, py - iy) > config_.max_corner_step)
				return fail(TrackFailureReason::JumpTooLarge);
		}
	std::vector<double> absolute;
	for (size_t i = 0; i < points.size(); ++i)
		if (usable[i])
			absolute.push_back(std::abs(residuals[i]));
	double const median = Median(absolute);
	double const cutoff = std::max(6.0, 3 * 1.4826 * median);
	double sum_t = 0, sum_v = 0, sum_tt = 0, sum_vv = 0, sum_tv = 0;
	size_t kept = 0;
	for (size_t i = 0; i < points.size(); ++i) {
		if (!usable[i] || std::abs(residuals[i]) > cutoff)
			continue;
		double const t = points[i].value, v = t + residuals[i];
		++kept;
		sum_t += t;
		sum_v += v;
		sum_tt += t * t;
		sum_vv += v * v;
		sum_tv += t * v;
	}
	if (static_cast<double>(kept) < (1 - config_.max_occlusion_fraction) * static_cast<double>(points.size()))
		return fail(TrackFailureReason::NccLow);
	double const denominator = std::sqrt(std::max(0.0,
												  (sum_tt - sum_t * sum_t / static_cast<double>(kept)) * (sum_vv - sum_v * sum_v / static_cast<double>(kept))));
	double const ncc = denominator > 1e-9 ? (sum_tv - sum_t * sum_v / static_cast<double>(kept)) / denominator : 0;
	if (ncc < config_.ncc_min)
		return fail(TrackFailureReason::NccLow);
	if (median > config_.residual_max)
		return fail(TrackFailureReason::ResidualHigh);
	result.status = TrackStatus::Ok;
	result.confidence = std::clamp(ncc, 0.0, 1.0);
	result.residual = median;
	result.candidate_center_x = request.search_center_x + radius * h[2];
	result.candidate_center_y = request.search_center_y + radius * h[5];
	// Remove the tracked center from the numerator before publishing the
	// shape. This also removes translation * perspective from its 2x2 part.
	result.transform.matrix = {h[0] - h[2] * h[6], h[1] - h[2] * h[7], 0,
							   h[3] - h[5] * h[6], h[4] - h[5] * h[7], 0, h[6] / radius, h[7] / radius, 1};
	return result;
}

} // namespace aegisub::motion_track
