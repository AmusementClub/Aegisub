#include "simd/bgra_transform.h"

#ifdef AEGISUB_WITH_HIGHWAY
#include "simd/bgra_transform_simd.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

struct FrameSpec {
	int width;
	int height;
	const char* label;
};

std::vector<unsigned char> MakeRandomFrame(int width, int height, ptrdiff_t stride, unsigned seed) {
	std::vector<unsigned char> data(static_cast<size_t>(stride) * height);
	std::mt19937 rng(seed);
	for (auto& b : data)
		b = static_cast<unsigned char>(rng());
	return data;
}

template <typename Transform>
double MedianMillis(Transform&& transform, std::vector<unsigned char> workspace_template,
                    int width, int height, ptrdiff_t stride, int runs) {
	std::vector<double> samples;
	samples.reserve(runs);
	for (int i = 0; i < runs; ++i) {
		auto data = workspace_template;  // fresh copy each run
		auto t0 = clock_type::now();
		transform(data, width, height, stride);
		auto t1 = clock_type::now();
		samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
	}
	std::sort(samples.begin(), samples.end());
	return samples[samples.size() / 2];
}

void RunScenario(const FrameSpec& spec, ptrdiff_t stride_extra, int runs) {
	const ptrdiff_t stride = static_cast<ptrdiff_t>(spec.width) * 4 + stride_extra;
	const size_t frame_bytes = static_cast<size_t>(spec.width) * spec.height * 4;
	auto template_frame = MakeRandomFrame(spec.width, spec.height, stride, 12345u);

	auto scalar_ms = MedianMillis(
	    [](std::vector<unsigned char>& d, int w, int h, ptrdiff_t s) {
		    aegisub::bgra::FlipHorizontal(d, w, h, s);
	    },
	    template_frame, spec.width, spec.height, stride, runs);

	double scalar_gbs = (frame_bytes / 1e9) / (scalar_ms / 1e3);

	std::printf("  %-18s %4dx%-4d stride=%-5zu  scalar: %7.3f ms  (%6.2f GB/s)\n",
	            spec.label, spec.width, spec.height, (size_t)stride, scalar_ms, scalar_gbs);

#ifdef AEGISUB_WITH_HIGHWAY
	auto simd_ms = MedianMillis(
	    [](std::vector<unsigned char>& d, int w, int h, ptrdiff_t s) {
		    aegisub::bgra::FlipHorizontalSimd(d, w, h, s);
	    },
	    template_frame, spec.width, spec.height, stride, runs);
	double simd_gbs = (frame_bytes / 1e9) / (simd_ms / 1e3);
	double speedup = scalar_ms / simd_ms;
	std::printf("  %-18s %4dx%-4d stride=%-5zu  SIMD:   %7.3f ms  (%6.2f GB/s)  speedup: %.2fx\n\n",
	            spec.label, spec.width, spec.height, (size_t)stride, simd_ms, simd_gbs, speedup);
#else
	std::printf("  (AEGISUB_WITH_HIGHWAY off -- no SIMD variant benchmarked)\n\n");
#endif
}

}  // namespace

int main() {
	const int runs = 11;
	const FrameSpec specs[] = {
	    {1920, 1080, "1080p"},
	    {1280, 720, "720p"},
	    {3840, 2160, "4K"},
	    {640, 480, "VGA"},
	    {1921, 1081, "odd"},
	};

	std::printf("BGRA FlipHorizontal benchmark (median of %d runs)\n", runs);
	std::printf("==================================================\n\n");

	for (ptrdiff_t extra : {(ptrdiff_t)0, (ptrdiff_t)64}) {
		std::printf("--- stride padding: %zd bytes ---\n", extra);
		for (const auto& spec : specs)
			RunScenario(spec, extra, runs);
	}

	return 0;
}
