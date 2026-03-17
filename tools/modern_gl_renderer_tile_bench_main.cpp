#include "../src/modern_gl_renderer_tile.h"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;

struct BenchResult {
	std::string name;
	double ns_per_op = 0.0;
};

template<typename Func>
BenchResult run_bench(char const* name, std::size_t iterations, Func&& func) {
	volatile std::uint64_t sink = 0;
	for (int i = 0; i < 8; ++i)
		sink += func();

	auto start = clock_type::now();
	for (std::size_t i = 0; i < iterations; ++i)
		sink += func();
	auto end = clock_type::now();

	(void)sink;
	double total_ns = std::chrono::duration<double, std::nano>(end - start).count();
	return {name, total_ns / iterations};
}

std::uint64_t bench_layout(int width, int height, int max_texture_size, bool supports_rectangles) {
	std::uint64_t sum = 0;
	for (int flipped = 0; flipped < 2; ++flipped) {
		auto layout = BuildModernGLTileLayout(width, height, 4, max_texture_size, supports_rectangles, flipped != 0);
		sum += static_cast<std::uint64_t>(layout.tiles.size()) * 17;
		for (auto const& tile : layout.tiles) {
			sum += static_cast<std::uint64_t>(tile.texture_w);
			sum += static_cast<std::uint64_t>(tile.texture_h);
			sum += static_cast<std::uint64_t>(tile.data_offset);
		}
	}
	return sum;
}

std::uint64_t bench_matrix(int width, int height) {
	std::uint64_t sum = 0;
	for (int flipped = 0; flipped < 2; ++flipped) {
		auto matrix = BuildModernGLOrthoMatrix(width, height, flipped != 0);
		for (float value : matrix)
			sum += static_cast<std::uint64_t>((value + 4.0f) * 1000000.0f);
	}
	return sum;
}
}

int main() {
	std::vector<BenchResult> results;
	results.push_back(run_bench("layout_1080p_rect", 20000, [] { return bench_layout(1920, 1080, 4096, true); }));
	results.push_back(run_bench("layout_4k_rect", 10000, [] { return bench_layout(3840, 2160, 4096, true); }));
	results.push_back(run_bench("layout_4k_square", 10000, [] { return bench_layout(3840, 2160, 2048, false); }));
	results.push_back(run_bench("matrix_4k", 500000, [] { return bench_matrix(3840, 2160); }));

	std::cout << "Modern GL renderer tile benchmark\n";
	std::cout << std::left << std::setw(24) << "name"
		<< std::right << std::setw(14) << "ns/op"
		<< "\n";
	for (auto const& result : results) {
		std::cout << std::left << std::setw(24) << result.name
			<< std::right << std::setw(14) << std::fixed << std::setprecision(2) << result.ns_per_op
			<< "\n";
	}

	return 0;
}
