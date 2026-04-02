#pragma once

#ifdef AEGISUB_WITH_HIGHWAY

#include <hwy/highway.h>

namespace aegisub::simd {
namespace hn = hwy::HWY_NAMESPACE;

template <class D>
HWY_INLINE auto Div255(const D d, hn::Vec<D> value) {
	return hn::ShiftRight<8>(hn::Add(hn::Add(value, hn::Set(d, 1)), hn::ShiftRight<8>(value)));
}

}  // namespace aegisub::simd

#endif
