#pragma once

// Byte/integer-exact with ZeroMeanNccScalar: all accumulators are integers
// (per-row int32 lane sums flushed to int64 per row), so results match the
// scalar reference bit-for-bit at every candidate offset. Follows the
// bgra_transform_simd.h single-target (HWY_NAMESPACE) style.

#include "../motion_track/ncc.h"

#ifdef AEGISUB_WITH_HIGHWAY

#include <hwy/highway.h>

#include <cmath>

namespace aegisub::motion_track::simd {
namespace hn = hwy::HWY_NAMESPACE;

struct WindowSums {
	long long sum_i = 0;
	long long sum_i2 = 0;
	long long sum_ti = 0;
};

// Accumulates exact raw sums of one aligned template row against the image
// row starting at offset ox. int32 lanes are safe per row: each product is
// <= 65025 and rows are capped far below overflow (>= 33k px).
inline void AccumulateRowSimd(
	std::uint8_t const* tpl_row, std::uint8_t const* img_row, int width,
	WindowSums& sums) {
	// Rebind keeps the lane count and narrows the element type, matching this
	// hwy version's PromoteTo(source must have target lane count) contract.
	// Exactness first: narrow vectors are fine here — the pyramid scan is the
	// dominant per-frame cost, and bit-exact integer accumulators matter more
	// than wider lanes.
	using D32 = hn::ScalableTag<int32_t>;
	using D16 = hn::Rebind<uint16_t, D32>;
	using D8 = hn::Rebind<uint8_t, D16>;
	D32 const d32;
	D16 const d16;
	D8 const d8;
	size_t const lanes8 = hn::Lanes(d8);
	int x = 0;

	hn::Vec<decltype(d32)> acc_i = hn::Zero(d32);
	hn::Vec<decltype(d32)> acc_i2 = hn::Zero(d32);
	hn::Vec<decltype(d32)> acc_ti = hn::Zero(d32);

	for (; x + static_cast<int>(lanes8) <= width;
	     x += static_cast<int>(lanes8)) {
		auto vb = hn::LoadU(d8, img_row + x);
		auto vt = hn::LoadU(d8, tpl_row + x);
		auto b32 = hn::PromoteTo(d32, hn::PromoteTo(d16, vb));
		auto t32 = hn::PromoteTo(d32, hn::PromoteTo(d16, vt));
		acc_i = hn::Add(acc_i, b32);
		acc_i2 = hn::Add(acc_i2, hn::Mul(b32, b32));
		acc_ti = hn::Add(acc_ti, hn::Mul(t32, b32));
	}

	sums.sum_i += hn::ReduceSum(d32, acc_i);
	sums.sum_i2 += hn::ReduceSum(d32, acc_i2);
	sums.sum_ti += hn::ReduceSum(d32, acc_ti);

	for (; x < width; ++x) {
		int const tv = tpl_row[x];
		int const iv = img_row[x];
		sums.sum_i += iv;
		sums.sum_i2 += iv * iv;
		sums.sum_ti += tv * iv;
	}
}

inline BestNccResult FindBestNccSimd(GrayView templ, GrayView image,
									 int min_ox, int min_oy, int max_ox, int max_oy, NccTieBreak tie = {}) {
	BestNccResult best;
	double best_ncc = -2.0;

	if (!templ.data || !image.data || templ.width <= 0 || templ.height <= 0
	    || templ.stride < templ.width || image.stride < image.width)
		return best;

	// Tie-break state mirroring ncc.cpp's scalar rule exactly: the decision
	// runs on the same bit-exact doubles this kernel produces, so both paths
	// pick identical offsets with or without the preference.
	// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
	// (ISC license).
	auto consider = [&](double ncc, int ox, int oy) {
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
	};

	// Template sums once; identical integer math to the scalar reference.
	long long sum_t = 0;
	long long sum_t2 = 0;
	for (int y = 0; y < templ.height; ++y) {
		auto const* row =
			templ.data + int64_t(y) * templ.stride;
		for (int x = 0; x < templ.width; ++x) {
			int const v = row[x];
			sum_t += v;
			sum_t2 += v * v;
		}
	}

	hn::ScalableTag<int32_t> d32;
	long long const n = int64_t(templ.width) * templ.height;

	for (int oy = min_oy; oy <= max_oy; ++oy) {
		if (oy < 0 || oy + templ.height > image.height)
			continue;
		for (int ox = min_ox; ox <= max_ox; ++ox) {
			if (ox < 0 || ox + templ.width > image.width)
				continue;

			WindowSums sums;
			for (int y = 0; y < templ.height; ++y) {
				auto const* tpl_row =
					templ.data + int64_t(y) * templ.stride;
				auto const* img_row =
					image.data + int64_t(oy + y) * image.stride + ox;
				AccumulateRowSimd(tpl_row, img_row, templ.width, sums);
			}

			long long const num = n * sums.sum_ti - sum_t * sums.sum_i;
			long long const factor_t = n * sum_t2 - sum_t * sum_t;
			long long const factor_i =
				n * sums.sum_i2 - sums.sum_i * sums.sum_i;
			if (factor_t < 0 || factor_i < 0)
				continue;
			double const denom =
				std::sqrt(double(factor_t)) * std::sqrt(double(factor_i));
			if (denom < 1e-6)
				continue;
			consider(double(num) / denom, ox, oy);
		}
	}
	return best;
}

} // namespace aegisub::motion_track::simd

#endif // AEGISUB_WITH_HIGHWAY
