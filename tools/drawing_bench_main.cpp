#include <libaegisub/ass/drawing.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using agi::ass::drawing::DrawingBooleanOp;
using agi::ass::drawing::DrawingStrokeCap;
using agi::ass::drawing::DrawingStrokeJoin;
using agi::ass::drawing::PathData;

volatile std::size_t sink = 0;

int ParseIterations(int argc, char **argv) {
	int iterations = 2000;
	for (int index = 1; index < argc; ++index) {
		std::string_view arg(argv[index]);
		if (arg == "--quick") {
			iterations = 200;
			continue;
		}
		if (arg == "--iterations" && index + 1 < argc) {
			iterations = std::max(1, std::atoi(argv[++index]));
			continue;
		}
	}
	return iterations;
}

template<typename Func>
void RunBench(std::string_view name, int iterations, Func&& func) {
	auto start = std::chrono::steady_clock::now();
	for (int iteration = 0; iteration < iterations; ++iteration)
		func(iteration);
	auto end = std::chrono::steady_clock::now();

	auto elapsed = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	double per_iteration = elapsed * 1000.0 / static_cast<double>(iterations);
	std::cout << name << ": " << elapsed << " ms total, " << per_iteration << " us/iter\n";
}

PathData Parse(std::string_view shape) {
	return agi::ass::drawing::ParseAss(std::string(shape));
}

PathData ParseOpen(std::string_view shape) {
	return agi::ass::drawing::ParseAssOpen(std::string(shape));
}

}

int main(int argc, char **argv) {
	int iterations = ParseIterations(argc, argv);

	std::vector<std::string> shapes = {
		"m 0 0 l 80 0 80 40 0 40",
		"m 0 0 b 20 -30 60 70 80 20 l 120 40 b 90 80 40 60 0 0",
		"m 0 0 l 80 0 80 80 0 80 m 60 20 l 60 60 20 60 20 20",
		"m 0 0 l 30 30 0 30 30 0",
		"m 1000000 1000000 l 1000100 1000000 1000100 1000100 1000000 1000100",
	};

	std::vector<PathData> filled;
	std::vector<PathData> open;
	filled.reserve(shapes.size());
	open.reserve(shapes.size());
	for (auto const& shape : shapes) {
		filled.push_back(Parse(shape));
		open.push_back(ParseOpen(shape));
	}

	std::cout << "Drawing benchmark\n";
	std::cout << "iterations: " << iterations << "\n";
	std::cout << "drawing_skia_backend: " << (agi::ass::drawing::DrawingSkiaBackendAvailable() ? "on" : "off") << "\n\n";

	RunBench("parse_serialize_filled", iterations, [&](int iteration) {
		auto path = agi::ass::drawing::ParseAss(shapes[static_cast<std::size_t>(iteration) % shapes.size()]);
		auto text = agi::ass::drawing::SerializeAssFilled(path);
		sink += text.size();
	});

	RunBench("compact_filled", iterations, [&](int iteration) {
		auto text = agi::ass::drawing::CompactAss(shapes[static_cast<std::size_t>(iteration) % shapes.size()]);
		sink += text.size();
	});

	RunBench("bounds_length_flatten", iterations, [&](int iteration) {
		auto const& path = open[static_cast<std::size_t>(iteration) % open.size()];
		agi::ass::drawing::Rect bounds;
		if (agi::ass::drawing::TryGetBounds(path, bounds))
			sink += static_cast<std::size_t>(bounds.width + bounds.height);
		sink += static_cast<std::size_t>(agi::ass::drawing::PathLength(path));
		sink += agi::ass::drawing::FlattenPath(path, 0.5).commands.size();
	});

	RunBench("transform_serialize_open", iterations, [&](int iteration) {
		auto path = agi::ass::drawing::TransformPath(open[static_cast<std::size_t>(iteration) % open.size()],
			{1.1, 0.05, -0.03, 0.9, 2.0, -3.0});
		auto text = agi::ass::drawing::SerializeAss(path);
		sink += text.size();
	});

	if (agi::ass::drawing::DrawingSkiaBackendAvailable()) {
		RunBench("skia_contains", iterations, [&](int iteration) {
			bool contains = false;
			auto const& path = filled[static_cast<std::size_t>(iteration) % filled.size()];
			agi::ass::drawing::TryDrawingContainsPoint(path,
				10.0 + static_cast<double>(iteration % 7),
				10.0 + static_cast<double>(iteration % 11),
				contains);
			sink += contains ? 1u : 0u;
		});

		RunBench("skia_boolean_union", iterations, [&](int iteration) {
			PathData result;
			auto const& lhs = filled[static_cast<std::size_t>(iteration) % filled.size()];
			auto const& rhs = filled[(static_cast<std::size_t>(iteration) + 1) % filled.size()];
			agi::ass::drawing::TryDrawingBoolean(lhs, rhs, DrawingBooleanOp::Union, result);
			sink += agi::ass::drawing::SerializeAssFilled(result).size();
		});

		RunBench("skia_outline", iterations, [&](int iteration) {
			PathData result;
			auto const& path = open[static_cast<std::size_t>(iteration) % open.size()];
			agi::ass::drawing::TryDrawingOutline(path,
				2.0 + static_cast<double>(iteration % 4),
				DrawingStrokeCap::Round,
				DrawingStrokeJoin::Round,
				result);
			sink += agi::ass::drawing::SerializeAssFilled(result).size();
		});
	} else {
		std::cout << "skia_contains: skipped\n";
		std::cout << "skia_boolean_union: skipped\n";
		std::cout << "skia_outline: skipped\n";
	}

	std::cout << "\nsink: " << sink << "\n";
	return 0;
}
