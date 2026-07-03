#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aegisub::bgra {

/// Mirror each row left-to-right. Operates in place on `data`.
/// `width`/`height` are pixel dimensions; `src_stride` is bytes per source
/// row (>= width*4). Equivalent to the original Flip > 0 loop.
inline void FlipHorizontal(std::vector<unsigned char>& data,
                           int width, int height, ptrdiff_t src_stride) {
	for (int x = 0; x < height; ++x)
		for (int y = 0; y < width / 2; ++y)
			for (int ch = 0; ch < 4; ++ch)
				std::swap(data[src_stride * x + 4 * y + ch],
				          data[src_stride * x + 4 * (width - 1 - y) + ch]);
}

/// Mirror top-to-bottom. Operates in place on `data`.
/// Equivalent to the original Flip < 0 loop.
inline void FlipVertical(std::vector<unsigned char>& data,
                         int width, int height, ptrdiff_t src_stride) {
	for (int x = 0; x < height / 2; ++x)
		for (int y = 0; y < width; ++y)
			for (int ch = 0; ch < 4; ++ch)
				std::swap(data[src_stride * x + 4 * y + ch],
				          data[src_stride * (height - 1 - x) + 4 * y + ch]);
}

/// Rotate 180 degrees. Produces a new tight buffer (out stride == width*4).
/// Output dimensions equal input dimensions.
inline std::vector<unsigned char> RotateHalfTurn(
		std::vector<unsigned char> data,
		int width, int height, ptrdiff_t src_stride) {
	std::vector<unsigned char> out(static_cast<size_t>(width) * height * 4);
	for (int x = 0; x < height; ++x)
		for (int y = 0; y < width; ++y)
			for (int ch = 0; ch < 4; ++ch)
				out[4 * (width * x + y) + ch] =
					data[src_stride * (height - 1 - x) + 4 * (width - 1 - y) + ch];
	return out;
}

/// Rotate 90 degrees clockwise. Output width/height are swapped; output
/// stride is tight (out_height*4 == former width*4).
inline std::vector<unsigned char> RotateQuarterClockwise(
		std::vector<unsigned char> data,
		int width, int height, ptrdiff_t src_stride) {
	std::vector<unsigned char> out(static_cast<size_t>(width) * height * 4);
	for (int x = 0; x < width; ++x)
		for (int y = 0; y < height; ++y)
			for (int ch = 0; ch < 4; ++ch)
				out[4 * (height * x + y) + ch] =
					data[src_stride * y + 4 * (width - 1 - x) + ch];
	return out;
}

/// Rotate 90 degrees counter-clockwise. Output width/height are swapped;
/// output stride is tight (out_height*4 == former width*4).
inline std::vector<unsigned char> RotateQuarterCounterClockwise(
		std::vector<unsigned char> data,
		int width, int height, ptrdiff_t src_stride) {
	std::vector<unsigned char> out(static_cast<size_t>(width) * height * 4);
	for (int x = 0; x < width; ++x)
		for (int y = 0; y < height; ++y)
			for (int ch = 0; ch < 4; ++ch)
				out[4 * (height * x + y) + ch] =
					data[src_stride * (height - 1 - y) + 4 * x + ch];
	return out;
}

}  // namespace aegisub::bgra
