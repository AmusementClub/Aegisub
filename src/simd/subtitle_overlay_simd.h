#pragma once

#include "subtitle_overlay_blend.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef AEGISUB_WITH_HIGHWAY
#include "simd/highway_utils.h"
#endif

namespace aegisub::simd {

namespace detail {

inline unsigned int AssR(std::uint32_t color) { return color >> 24; }
inline unsigned int AssG(std::uint32_t color) { return (color >> 16) & 0xFF; }
inline unsigned int AssB(std::uint32_t color) { return (color >> 8) & 0xFF; }
inline unsigned int AssA(std::uint32_t color) { return color & 0xFF; }

#ifdef AEGISUB_WITH_HIGHWAY
template <class D>
HWY_INLINE auto ExtractBlueChannel(D d, hn::Vec<D> pixels) {
	return hn::And(pixels, hn::Set(d, 0xFFu));
}

template <class D>
HWY_INLINE auto ExtractGreenChannel(D d, hn::Vec<D> pixels) {
	return hn::And(hn::ShiftRight<8>(pixels), hn::Set(d, 0xFFu));
}

template <class D>
HWY_INLINE auto ExtractRedChannel(D d, hn::Vec<D> pixels) {
	return hn::And(hn::ShiftRight<16>(pixels), hn::Set(d, 0xFFu));
}

template <class D>
HWY_INLINE auto ExtractAlphaChannel(D d, hn::Vec<D> pixels) {
	return hn::And(hn::ShiftRight<24>(pixels), hn::Set(d, 0xFFu));
}

template <class D>
HWY_INLINE auto PackBgraPixels(
	D d,
	hn::Vec<D> blue,
	hn::Vec<D> green,
	hn::Vec<D> red,
	hn::Vec<D> alpha) {
	return hn::Or(
		blue,
		hn::Or(
			hn::ShiftLeft<8>(green),
			hn::Or(hn::ShiftLeft<16>(red), hn::ShiftLeft<24>(alpha))));
}
#endif

}  // namespace detail

inline SubtitleOverlayRowRange BuildSparseOverlayRow(
	unsigned char const* src_row,
	unsigned char const* composited_row,
	unsigned char* dst_row,
	int width) {
	if (!src_row || !composited_row || !dst_row || width <= 0)
		return {};

	const size_t row_bytes = static_cast<size_t>(width) * 4;
	if (std::memcmp(src_row, composited_row, row_bytes) == 0)
		return {};

	int row_x0 = width;
	int row_x1 = 0;

#ifdef AEGISUB_WITH_HIGHWAY
	const hn::CappedTag<uint32_t, 8> d32;
	const size_t lanes = hn::Lanes(d32);
	const auto zero = hn::Zero(d32);
	const auto alpha_mask = hn::Set(d32, 0xFF000000u);
	std::array<uint32_t, 8> sparse_pixels {};
	const int simd_width = width - (width % static_cast<int>(lanes));
	int x = 0;
	for (; x < simd_width; x += static_cast<int>(lanes)) {
		auto src = reinterpret_cast<uint32_t const*>(src_row + static_cast<ptrdiff_t>(x) * 4);
		auto composited = reinterpret_cast<uint32_t const*>(composited_row + static_cast<ptrdiff_t>(x) * 4);
		auto src_vec = hn::LoadU(d32, src);
		auto composited_vec = hn::LoadU(d32, composited);
		auto equal_mask = hn::Eq(src_vec, composited_vec);
		if (hn::AllTrue(d32, equal_mask))
			continue;

		auto sparse = hn::IfThenElse(equal_mask, zero, hn::Or(composited_vec, alpha_mask));
		hn::StoreU(sparse, d32, sparse_pixels.data());
		std::memcpy(dst_row + static_cast<ptrdiff_t>(x) * 4, sparse_pixels.data(), lanes * sizeof(uint32_t));
		for (size_t lane = 0; lane < lanes; ++lane) {
			if (!sparse_pixels[lane])
				continue;
			row_x0 = std::min(row_x0, x + static_cast<int>(lane));
			row_x1 = std::max(row_x1, x + static_cast<int>(lane) + 1);
		}
	}
#else
	int x = 0;
#endif

	for (; x < width; ++x) {
		auto pixel_offset = static_cast<ptrdiff_t>(x) * 4;
		auto const* src = src_row + pixel_offset;
		auto const* composited = composited_row + pixel_offset;
		auto* dst = dst_row + pixel_offset;
		if (std::memcmp(src, composited, 4) == 0)
			continue;

		dst[0] = composited[0];
		dst[1] = composited[1];
		dst[2] = composited[2];
		dst[3] = 255;
		row_x0 = std::min(row_x0, x);
		row_x1 = x + 1;
	}

	if (row_x0 >= row_x1)
		return {};
	return { row_x0, row_x1 };
}

inline void BlendLibassMaskRow(
	unsigned char* dst,
	unsigned char const* mask,
	int width,
	SubtitleOverlayBlendMode mode,
	std::uint32_t ass_color) {
	if (!dst || !mask || width <= 0)
		return;

	const unsigned int opacity = 255 - detail::AssA(ass_color);
	const unsigned int r = detail::AssR(ass_color);
	const unsigned int g = detail::AssG(ass_color);
	const unsigned int b = detail::AssB(ass_color);

#ifdef AEGISUB_WITH_HIGHWAY
	const hn::CappedTag<uint32_t, 8> d32;
	const hn::Rebind<uint8_t, decltype(d32)> d8;
	const size_t lanes = hn::Lanes(d32);
	const auto zero = hn::Zero(d32);
	const auto alpha_255 = hn::Set(d32, 255u);
	const auto opacity_vec = hn::Set(d32, opacity);
	const auto color_b = hn::Set(d32, b);
	const auto color_g = hn::Set(d32, g);
	const auto color_r = hn::Set(d32, r);

	int x = 0;
	const int simd_width = width - (width % static_cast<int>(lanes));
	for (; x < simd_width; x += static_cast<int>(lanes)) {
		auto mask_alpha = hn::PromoteTo(d32, hn::LoadU(d8, mask + x));
		auto src_alpha = Div255(d32, hn::Mul(mask_alpha, opacity_vec));
		if (hn::AllTrue(d32, hn::Eq(src_alpha, zero)))
			continue;

		auto dst_pixels = hn::LoadU(d32, reinterpret_cast<uint32_t const*>(dst + static_cast<ptrdiff_t>(x) * 4));
		auto inv_alpha = hn::Sub(alpha_255, src_alpha);
		auto dst_b = detail::ExtractBlueChannel(d32, dst_pixels);
		auto dst_g = detail::ExtractGreenChannel(d32, dst_pixels);
		auto dst_r = detail::ExtractRedChannel(d32, dst_pixels);
		auto out_a = zero;
		auto out_b = zero;
		auto out_g = zero;
		auto out_r = zero;

		if (mode == SubtitleOverlayBlendMode::LegacyBakeIn) {
			out_b = Div255(d32, hn::Add(hn::Mul(src_alpha, color_b), hn::Mul(inv_alpha, dst_b)));
			out_g = Div255(d32, hn::Add(hn::Mul(src_alpha, color_g), hn::Mul(inv_alpha, dst_g)));
			out_r = Div255(d32, hn::Add(hn::Mul(src_alpha, color_r), hn::Mul(inv_alpha, dst_r)));
		}
		else {
			auto dst_a = detail::ExtractAlphaChannel(d32, dst_pixels);
			auto src_b = Div255(d32, hn::Mul(src_alpha, color_b));
			auto src_g = Div255(d32, hn::Mul(src_alpha, color_g));
			auto src_r = Div255(d32, hn::Mul(src_alpha, color_r));
			out_b = hn::Add(src_b, Div255(d32, hn::Mul(dst_b, inv_alpha)));
			out_g = hn::Add(src_g, Div255(d32, hn::Mul(dst_g, inv_alpha)));
			out_r = hn::Add(src_r, Div255(d32, hn::Mul(dst_r, inv_alpha)));
			out_a = hn::Add(src_alpha, Div255(d32, hn::Mul(dst_a, inv_alpha)));
		}

		auto result = detail::PackBgraPixels(d32, out_b, out_g, out_r, out_a);
		hn::StoreU(result, d32, reinterpret_cast<uint32_t*>(dst + static_cast<ptrdiff_t>(x) * 4));
	}
#else
	int x = 0;
#endif

	for (; x < width; ++x) {
		unsigned int src_alpha = static_cast<unsigned int>(mask[x]) * opacity / 255;
		if (!src_alpha)
			continue;

		auto* pixel = dst + static_cast<ptrdiff_t>(x) * 4;
		if (mode == SubtitleOverlayBlendMode::LegacyBakeIn) {
			unsigned int inv_alpha = 255 - src_alpha;
			pixel[0] = static_cast<unsigned char>((src_alpha * b + inv_alpha * pixel[0]) / 255);
			pixel[1] = static_cast<unsigned char>((src_alpha * g + inv_alpha * pixel[1]) / 255);
			pixel[2] = static_cast<unsigned char>((src_alpha * r + inv_alpha * pixel[2]) / 255);
			pixel[3] = 0;
		}
		else {
			unsigned int inv_alpha = 255 - src_alpha;
			unsigned int src_b = src_alpha * b / 255;
			unsigned int src_g = src_alpha * g / 255;
			unsigned int src_r = src_alpha * r / 255;
			pixel[0] = static_cast<unsigned char>(src_b + pixel[0] * inv_alpha / 255);
			pixel[1] = static_cast<unsigned char>(src_g + pixel[1] * inv_alpha / 255);
			pixel[2] = static_cast<unsigned char>(src_r + pixel[2] * inv_alpha / 255);
			pixel[3] = static_cast<unsigned char>(src_alpha + pixel[3] * inv_alpha / 255);
		}
	}
}

inline void CompositePremultipliedBgraRow(unsigned char* dst, unsigned char const* src, int width) {
	if (!dst || !src || width <= 0)
		return;

#ifdef AEGISUB_WITH_HIGHWAY
	const hn::CappedTag<uint32_t, 8> d32;
	const size_t lanes = hn::Lanes(d32);
	const auto zero = hn::Zero(d32);
	const auto alpha_255 = hn::Set(d32, 255u);

	int x = 0;
	const int simd_width = width - (width % static_cast<int>(lanes));
	for (; x < simd_width; x += static_cast<int>(lanes)) {
		auto src_pixels = hn::LoadU(d32, reinterpret_cast<uint32_t const*>(src + static_cast<ptrdiff_t>(x) * 4));
		auto src_alpha = detail::ExtractAlphaChannel(d32, src_pixels);
		if (hn::AllTrue(d32, hn::Eq(src_alpha, zero)))
			continue;

		auto dst_pixels = hn::LoadU(d32, reinterpret_cast<uint32_t const*>(dst + static_cast<ptrdiff_t>(x) * 4));
		auto inv_alpha = hn::Sub(alpha_255, src_alpha);
		auto out_b = hn::Add(
			detail::ExtractBlueChannel(d32, src_pixels),
			Div255(d32, hn::Mul(detail::ExtractBlueChannel(d32, dst_pixels), inv_alpha)));
		auto out_g = hn::Add(
			detail::ExtractGreenChannel(d32, src_pixels),
			Div255(d32, hn::Mul(detail::ExtractGreenChannel(d32, dst_pixels), inv_alpha)));
		auto out_r = hn::Add(
			detail::ExtractRedChannel(d32, src_pixels),
			Div255(d32, hn::Mul(detail::ExtractRedChannel(d32, dst_pixels), inv_alpha)));
		auto result = detail::PackBgraPixels(d32, out_b, out_g, out_r, zero);
		hn::StoreU(result, d32, reinterpret_cast<uint32_t*>(dst + static_cast<ptrdiff_t>(x) * 4));
	}
#else
	int x = 0;
#endif

	for (; x < width; ++x) {
		auto const* src_pixel = src + static_cast<ptrdiff_t>(x) * 4;
		auto* dst_pixel = dst + static_cast<ptrdiff_t>(x) * 4;
		unsigned int src_alpha = src_pixel[3];
		if (!src_alpha)
			continue;

		unsigned int inv_alpha = 255 - src_alpha;
		dst_pixel[0] = static_cast<unsigned char>(src_pixel[0] + dst_pixel[0] * inv_alpha / 255);
		dst_pixel[1] = static_cast<unsigned char>(src_pixel[1] + dst_pixel[1] * inv_alpha / 255);
		dst_pixel[2] = static_cast<unsigned char>(src_pixel[2] + dst_pixel[2] * inv_alpha / 255);
		dst_pixel[3] = 0;
	}
}

}  // namespace aegisub::simd
