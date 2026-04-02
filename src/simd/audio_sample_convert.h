#pragma once

#include <cstddef>
#include <cstdint>

#ifdef AEGISUB_WITH_HIGHWAY
#include "simd/highway_utils.h"
#endif

namespace aegisub::simd {

inline void DecodeUInt8ToFloat(uint8_t const* src, size_t sample_count, float* dst) {
#ifdef AEGISUB_WITH_HIGHWAY
	const hn::CappedTag<uint32_t, 8> du32;
	const hn::Rebind<uint8_t, decltype(du32)> du8;
	const hn::Rebind<float, decltype(du32)> df;
	const auto bias = hn::Set(df, 128.0f);
	const auto scale = hn::Set(df, 1.0f / 128.0f);
	const size_t lanes = hn::Lanes(du32);
	size_t i = 0;
	for (; i + lanes <= sample_count; i += lanes) {
		auto values = hn::PromoteTo(du32, hn::LoadU(du8, src + i));
		auto floats = hn::Mul(hn::Sub(hn::ConvertTo(df, values), bias), scale);
		hn::StoreU(floats, df, dst + i);
	}
	for (; i < sample_count; ++i)
		dst[i] = static_cast<float>(static_cast<int>(src[i]) - 128) / 128.0f;
#else
	for (size_t i = 0; i < sample_count; ++i)
		dst[i] = static_cast<float>(static_cast<int>(src[i]) - 128) / 128.0f;
#endif
}

inline void DecodeInt16ToFloat(int16_t const* src, size_t sample_count, float* dst) {
#ifdef AEGISUB_WITH_HIGHWAY
	const hn::CappedTag<int32_t, 8> di32;
	const hn::Rebind<int16_t, decltype(di32)> di16;
	const hn::Rebind<float, decltype(di32)> df;
	const auto scale = hn::Set(df, 1.0f / 32768.0f);
	const size_t lanes = hn::Lanes(di32);
	size_t i = 0;
	for (; i + lanes <= sample_count; i += lanes) {
		auto values = hn::PromoteTo(di32, hn::LoadU(di16, src + i));
		auto floats = hn::Mul(hn::ConvertTo(df, values), scale);
		hn::StoreU(floats, df, dst + i);
	}
	for (; i < sample_count; ++i)
		dst[i] = static_cast<float>(src[i]) / 32768.0f;
#else
	for (size_t i = 0; i < sample_count; ++i)
		dst[i] = static_cast<float>(src[i]) / 32768.0f;
#endif
}

inline void DecodeInt32ToFloat(int32_t const* src, size_t sample_count, float* dst) {
#ifdef AEGISUB_WITH_HIGHWAY
	const hn::CappedTag<int32_t, 8> di32;
	const hn::Rebind<float, decltype(di32)> df;
	const auto scale = hn::Set(df, 1.0f / 2147483648.0f);
	const size_t lanes = hn::Lanes(di32);
	size_t i = 0;
	for (; i + lanes <= sample_count; i += lanes) {
		auto values = hn::LoadU(di32, src + i);
		auto floats = hn::Mul(hn::ConvertTo(df, values), scale);
		hn::StoreU(floats, df, dst + i);
	}
	for (; i < sample_count; ++i)
		dst[i] = static_cast<float>(src[i] / 2147483648.0);
#else
	for (size_t i = 0; i < sample_count; ++i)
		dst[i] = static_cast<float>(src[i] / 2147483648.0);
#endif
}

inline void DecodeFloat64ToFloat(double const* src, size_t sample_count, float* dst) {
#ifdef AEGISUB_WITH_HIGHWAY
	const hn::CappedTag<double, 4> df64;
	const hn::Rebind<float, decltype(df64)> df32;
	const size_t lanes = hn::Lanes(df64);
	size_t i = 0;
	for (; i + lanes <= sample_count; i += lanes) {
		auto values = hn::LoadU(df64, src + i);
		auto floats = hn::DemoteTo(df32, values);
		hn::StoreU(floats, df32, dst + i);
	}
	for (; i < sample_count; ++i)
		dst[i] = static_cast<float>(src[i]);
#else
	for (size_t i = 0; i < sample_count; ++i)
		dst[i] = static_cast<float>(src[i]);
#endif
}

}  // namespace aegisub::simd
