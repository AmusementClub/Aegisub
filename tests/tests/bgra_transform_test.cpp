#include <main.h>

#include "../../src/simd/bgra_transform.h"

#ifdef AEGISUB_WITH_HIGHWAY
#include "../../src/simd/bgra_transform_simd.h"
#endif

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace {

struct BgraPixel {
	unsigned char b, g, r, a;
};
static_assert(sizeof(BgraPixel) == 4, "BGRA pixel must be 4 bytes");

// Build a BGRA frame of width x height with a unique, recoverable value per
// pixel (its linear source index), optionally with extra bytes of row
// padding so that stride > width*4. The padding bytes are filled too so that
// a transform that accidentally reads padding is caught when compared
// against a reference that only ever reads valid pixels.
std::vector<unsigned char> MakeFrame(int width, int height, ptrdiff_t stride, unsigned seed) {
	std::vector<unsigned char> data(static_cast<size_t>(stride) * height);
	std::mt19937 rng(seed);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			auto& px = *reinterpret_cast<BgraPixel*>(data.data() + y * stride + x * 4);
			// Encode source (x,y) into the channels so the reference can verify
			// each destination pixel came from the right source pixel.
			unsigned idx = static_cast<unsigned>(y * width + x);
			px.b = static_cast<unsigned char>(idx & 0xFF);
			px.g = static_cast<unsigned char>((idx >> 8) & 0xFF);
			px.r = static_cast<unsigned char>((idx >> 16) & 0xFF);
			px.a = static_cast<unsigned char>(((idx >> 24) & 0x7F) | 0x80);
			(void)rng;
		}
		// Fill row padding with a sentinel.
		for (ptrdiff_t p = static_cast<ptrdiff_t>(width) * 4; p < stride; ++p)
			data[static_cast<size_t>(y * stride + p)] = 0xAE;
	}
	return data;
}

// Decode a BGRA pixel's encoded source index (inverse of MakeFrame).
unsigned DecodeIndex(const BgraPixel& px) {
	return static_cast<unsigned>(px.b)
	     | (static_cast<unsigned>(px.g) << 8)
	     | (static_cast<unsigned>(px.r) << 16)
	     | ((static_cast<unsigned>(px.a) & 0x7F) << 24);
}

const BgraPixel& PixelAt(const std::vector<unsigned char>& data, int x, int y,
                         ptrdiff_t stride, int /*width*/) {
	return *reinterpret_cast<const BgraPixel*>(data.data() + y * stride + x * 4);
}

BgraPixel& PixelAt(std::vector<unsigned char>& data, int x, int y,
                   ptrdiff_t stride, int /*width*/) {
	return *reinterpret_cast<BgraPixel*>(data.data() + y * stride + x * 4);
}

}  // namespace

// ---------------------------------------------------------------------------
// FlipHorizontal
// ---------------------------------------------------------------------------

TEST(BgraTransform, FlipHorizontalMatchesReference) {
	for (int width : {1, 2, 3, 7, 16, 17, 64}) {
		for (int height : {1, 2, 5, 16}) {
			for (ptrdiff_t extra : {(ptrdiff_t)0, (ptrdiff_t)12}) {
				ptrdiff_t stride = static_cast<ptrdiff_t>(width) * 4 + extra;
				auto data = MakeFrame(width, height, stride, 1000u + width * 31 + height + extra);
				aegisub::bgra::FlipHorizontal(data, width, height, stride);

				// Reference: dst(x,y) == src(width-1-x, y).
				auto ref = MakeFrame(width, height, stride, 1000u + width * 31 + height + extra);
				for (int y = 0; y < height; ++y)
					for (int x = 0; x < width / 2; ++x)
						for (int ch = 0; ch < 4; ++ch)
							std::swap(ref[y * stride + x * 4 + ch],
							          ref[y * stride + (width - 1 - x) * 4 + ch]);

				EXPECT_EQ(data, ref) << "FlipHorizontal w=" << width << " h=" << height << " extra=" << extra;
			}
		}
	}
}

TEST(BgraTransform, FlipHorizontalMapsCorrectSourcePixel) {
	const int width = 9, height = 5;
	const ptrdiff_t stride = width * 4;
	auto ref = MakeFrame(width, height, stride, 42u);
	auto data = ref;
	aegisub::bgra::FlipHorizontal(data, width, height, stride);
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
			EXPECT_EQ(DecodeIndex(PixelAt(data, x, y, stride, width)),
			          static_cast<unsigned>(y * width + (width - 1 - x)))
			    << "at dst (" << x << "," << y << ")";
}

// ---------------------------------------------------------------------------
// FlipVertical
// ---------------------------------------------------------------------------

TEST(BgraTransform, FlipVerticalMatchesReference) {
	for (int width : {1, 2, 7, 16, 17}) {
		for (int height : {1, 2, 3, 8}) {
			for (ptrdiff_t extra : {(ptrdiff_t)0, (ptrdiff_t)8}) {
				ptrdiff_t stride = static_cast<ptrdiff_t>(width) * 4 + extra;
				auto data = MakeFrame(width, height, stride, 2000u + width * 17 + height + extra);
				aegisub::bgra::FlipVertical(data, width, height, stride);

				auto ref = MakeFrame(width, height, stride, 2000u + width * 17 + height + extra);
				for (int x = 0; x < height / 2; ++x)
					for (int y = 0; y < width; ++y)
						for (int ch = 0; ch < 4; ++ch)
							std::swap(ref[x * stride + y * 4 + ch],
							          ref[(height - 1 - x) * stride + y * 4 + ch]);

				EXPECT_EQ(data, ref) << "FlipVertical w=" << width << " h=" << height << " extra=" << extra;
			}
		}
	}
}

TEST(BgraTransform, FlipVerticalMapsCorrectSourcePixel) {
	const int width = 6, height = 7;
	const ptrdiff_t stride = width * 4;
	auto ref = MakeFrame(width, height, stride, 7u);
	auto data = ref;
	aegisub::bgra::FlipVertical(data, width, height, stride);
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
			EXPECT_EQ(DecodeIndex(PixelAt(data, x, y, stride, width)),
			          static_cast<unsigned>((height - 1 - y) * width + x))
			    << "at dst (" << x << "," << y << ")";
}

// ---------------------------------------------------------------------------
// RotateHalfTurn (180)
// ---------------------------------------------------------------------------

TEST(BgraTransform, RotateHalfTurnMatchesReference) {
	for (int width : {1, 2, 3, 9, 16}) {
		for (int height : {1, 2, 5, 13}) {
			for (ptrdiff_t extra : {(ptrdiff_t)0, (ptrdiff_t)20}) {
				ptrdiff_t stride = static_cast<ptrdiff_t>(width) * 4 + extra;
				auto src = MakeFrame(width, height, stride, 3000u + width * 7 + height + extra);
				auto out = aegisub::bgra::RotateHalfTurn(src, width, height, stride);

				// Reference: dst(x,y) == src(width-1-x, height-1-y).
				std::vector<unsigned char> ref(static_cast<size_t>(width) * height * 4);
				for (int y = 0; y < height; ++y)
					for (int x = 0; x < width; ++x)
						for (int ch = 0; ch < 4; ++ch)
							ref[(y * width + x) * 4 + ch] =
							    src[(height - 1 - y) * stride + (width - 1 - x) * 4 + ch];

				EXPECT_EQ(out, ref) << "RotateHalfTurn w=" << width << " h=" << height << " extra=" << extra;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// RotateQuarterClockwise (90 CW)
// Output dims: width_out = height, height_out = width.
// ---------------------------------------------------------------------------

TEST(BgraTransform, RotateQuarterClockwiseMatchesReference) {
	for (int width : {1, 2, 5, 12}) {
		for (int height : {1, 3, 8, 17}) {
			for (ptrdiff_t extra : {(ptrdiff_t)0, (ptrdiff_t)16}) {
				ptrdiff_t stride = static_cast<ptrdiff_t>(width) * 4 + extra;
				auto src = MakeFrame(width, height, stride, 4000u + width * 5 + height + extra);
				auto out = aegisub::bgra::RotateQuarterClockwise(src, width, height, stride);

				const int out_w = height, out_h = width;
				const ptrdiff_t out_stride = static_cast<ptrdiff_t>(out_w) * 4;
				ASSERT_EQ(out.size(), static_cast<size_t>(out_stride) * out_h);

				// Reference derived from the pixel-mapping semantics:
				// A 90 CW rotation sends source pixel (sx, sy) to
				// (sy, height-1-sx) in destination space.
				std::vector<unsigned char> ref(out.size());
				for (int sy = 0; sy < height; ++sy)
					for (int sx = 0; sx < width; ++sx) {
						int dx = sy;
						int dy = width - 1 - sx;
						for (int ch = 0; ch < 4; ++ch)
							ref[dy * out_stride + dx * 4 + ch] = src[sy * stride + sx * 4 + ch];
					}

				EXPECT_EQ(out, ref) << "RotateQuarterClockwise w=" << width << " h=" << height << " extra=" << extra;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// RotateQuarterCounterClockwise (90 CCW)
// Output dims: width_out = height, height_out = width.
// ---------------------------------------------------------------------------

TEST(BgraTransform, RotateQuarterCounterClockwiseMatchesReference) {
	for (int width : {1, 2, 4, 11}) {
		for (int height : {1, 3, 9, 15}) {
			for (ptrdiff_t extra : {(ptrdiff_t)0, (ptrdiff_t)24}) {
				ptrdiff_t stride = static_cast<ptrdiff_t>(width) * 4 + extra;
				auto src = MakeFrame(width, height, stride, 5000u + width * 3 + height + extra);
				auto out = aegisub::bgra::RotateQuarterCounterClockwise(src, width, height, stride);

				const int out_w = height, out_h = width;
				const ptrdiff_t out_stride = static_cast<ptrdiff_t>(out_w) * 4;
				ASSERT_EQ(out.size(), static_cast<size_t>(out_stride) * out_h);

				// Reference: a 90 CCW rotation sends source pixel (sx, sy) to
				// (height-1-sy, sx) in destination space.
				std::vector<unsigned char> ref(out.size());
				for (int sy = 0; sy < height; ++sy)
					for (int sx = 0; sx < width; ++sx) {
						int dx = height - 1 - sy;
						int dy = sx;
						for (int ch = 0; ch < 4; ++ch)
							ref[dy * out_stride + dx * 4 + ch] = src[sy * stride + sx * 4 + ch];
					}

				EXPECT_EQ(out, ref) << "RotateQuarterCounterClockwise w=" << width << " h=" << height << " extra=" << extra;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Identity / no-op sanity: applying FlipHorizontal twice returns to start.
// ---------------------------------------------------------------------------

TEST(BgraTransform, FlipHorizontalIsItsOwnInverse) {
	const int width = 13, height = 9;
	const ptrdiff_t stride = width * 4 + 4;
	auto original = MakeFrame(width, height, stride, 99u);
	auto data = original;
	aegisub::bgra::FlipHorizontal(data, width, height, stride);
	aegisub::bgra::FlipHorizontal(data, width, height, stride);
	EXPECT_EQ(data, original);
}

TEST(BgraTransform, FlipVerticalIsItsOwnInverse) {
	const int width = 11, height = 8;
	const ptrdiff_t stride = width * 4 + 6;
	auto original = MakeFrame(width, height, stride, 98u);
	auto data = original;
	aegisub::bgra::FlipVertical(data, width, height, stride);
	aegisub::bgra::FlipVertical(data, width, height, stride);
	EXPECT_EQ(data, original);
}

// ===========================================================================
// SIMD consistency: FlipHorizontalSimd must be byte-identical to the scalar
// FlipHorizontal across many geometries, including odd widths (which stress
// the vector tail) and padded strides.
// ===========================================================================
#ifdef AEGISUB_WITH_HIGHWAY

TEST(BgraTransformSimdConsistency, FlipHorizontalMatchesScalar) {
	// Widths below, at, and above common SIMD lane counts (4/8 for SSE/AVX2),
	// plus odd values to exercise the scalar tail.
	for (int width : {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 1920}) {
		for (int height : {1, 2, 3, 8, 1080}) {
			for (ptrdiff_t extra : {(ptrdiff_t)0, (ptrdiff_t)4, (ptrdiff_t)64}) {
				ptrdiff_t stride = static_cast<ptrdiff_t>(width) * 4 + extra;
				auto ref = MakeFrame(width, height, stride, 7000u + width * 13 + height + extra);
				auto simd = ref;

				aegisub::bgra::FlipHorizontal(ref, width, height, stride);
				aegisub::bgra::FlipHorizontalSimd(simd, width, height, stride);

				EXPECT_EQ(simd, ref)
				    << "width=" << width << " height=" << height << " extra=" << extra;
			}
		}
	}
}

TEST(BgraTransformSimdConsistency, FlipHorizontalSimdIsItsOwnInverse) {
	const int width = 100, height = 50;
	const ptrdiff_t stride = width * 4 + 8;
	auto original = MakeFrame(width, height, stride, 71u);
	auto data = original;
	aegisub::bgra::FlipHorizontalSimd(data, width, height, stride);
	aegisub::bgra::FlipHorizontalSimd(data, width, height, stride);
	EXPECT_EQ(data, original);
}

#endif  // AEGISUB_WITH_HIGHWAY
