#include "../src/subtitle_overlay_blend.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;

struct BenchResult {
	std::string name;
	double ns_per_op = 0.0;
	double rel = 0.0;
	std::size_t storage_bytes = 0;
	std::size_t upload_bytes = 0;
};

template<typename Func>
BenchResult run_bench(char const* name, std::size_t iterations, Func&& func) {
	volatile std::uint64_t sink = 0;
	std::size_t storage_bytes = 0;
	std::size_t upload_bytes = 0;

	for (int i = 0; i < 4; ++i) {
		auto result = func();
		sink += result.first;
		storage_bytes = result.second.first;
		upload_bytes = result.second.second;
	}

	auto start = clock_type::now();
	for (std::size_t i = 0; i < iterations; ++i) {
		auto result = func();
		sink += result.first;
		storage_bytes = result.second.first;
		upload_bytes = result.second.second;
	}
	auto end = clock_type::now();

	(void)sink;
	double total_ns = std::chrono::duration<double, std::nano>(end - start).count();
	return {name, total_ns / iterations, 0.0, storage_bytes, upload_bytes};
}

void print_group(char const* title, std::vector<BenchResult> results) {
	std::sort(results.begin(), results.end(), [](BenchResult const& a, BenchResult const& b) {
		return a.ns_per_op < b.ns_per_op;
	});

	double best = results.front().ns_per_op;
	std::cout << "\n" << title << "\n";
	std::cout << std::left << std::setw(32) << "name"
		<< std::right << std::setw(14) << "ns/op"
		<< std::setw(10) << "rel"
		<< std::setw(16) << "storage"
		<< std::setw(16) << "upload"
		<< "\n";

	for (auto& result : results) {
		result.rel = result.ns_per_op / best;
		std::cout << std::left << std::setw(32) << result.name
			<< std::right << std::setw(14) << std::fixed << std::setprecision(2) << result.ns_per_op
			<< std::setw(10) << std::fixed << std::setprecision(2) << result.rel
			<< std::setw(16) << result.storage_bytes
			<< std::setw(16) << result.upload_bytes
			<< "\n";
	}
}

std::size_t dirty_upload_bytes(SubtitleOverlay const& overlay) {
	if (overlay.dirty_rect_count <= 0 || !overlay.dirty_rects)
		return 0;

	std::size_t total = 0;
	for (int i = 0; i < overlay.dirty_rect_count; ++i)
		total += static_cast<std::size_t>(overlay.dirty_rects[i].width) * overlay.dirty_rects[i].height * 4;
	return total;
}

std::size_t overlay_storage_bytes(SubtitleOverlayStorage const& storage) {
	return storage.pixels.size()
		+ storage.row_ranges.size() * sizeof(SubtitleOverlayRowRange)
		+ storage.dirty_rects.size() * sizeof(SubtitleOverlayDirtyRect);
}

VideoFrame make_frame(int width, int height) {
	VideoFrame frame;
	frame.width = static_cast<std::size_t>(width);
	frame.height = static_cast<std::size_t>(height);
	frame.pitch = static_cast<std::size_t>(width) * 4;
	frame.flipped = false;
	frame.data.assign(frame.pitch * frame.height, 0);
	return frame;
}

void fill_box(VideoFrame& frame, int x0, int y0, int width, int height, unsigned char b, unsigned char g, unsigned char r) {
	for (int y = y0; y < y0 + height; ++y) {
		auto* row = frame.data.data() + static_cast<std::ptrdiff_t>(y) * frame.pitch;
		for (int x = x0; x < x0 + width; ++x) {
			auto* pixel = row + static_cast<std::ptrdiff_t>(x) * 4;
			pixel[0] = b;
			pixel[1] = g;
			pixel[2] = r;
			pixel[3] = 0;
		}
	}
}

std::pair<VideoFrame, VideoFrame> make_single_patch_case() {
	auto source = make_frame(1920, 1080);
	auto composited = source;
	fill_box(composited, 860, 900, 220, 80, 220, 220, 220);
	return {std::move(source), std::move(composited)};
}

std::pair<VideoFrame, VideoFrame> make_sparse_case() {
	auto source = make_frame(1920, 1080);
	auto composited = source;
	fill_box(composited, 120, 120, 140, 40, 255, 255, 255);
	fill_box(composited, 1440, 160, 220, 44, 255, 255, 255);
	fill_box(composited, 640, 920, 300, 48, 255, 255, 255);
	fill_box(composited, 1320, 970, 160, 40, 180, 220, 255);
	return {std::move(source), std::move(composited)};
}

struct TransitionCase {
	VideoFrame previous_source;
	VideoFrame previous_composited;
	VideoFrame current_source;
	VideoFrame current_composited;
};

TransitionCase make_background_change_case() {
	auto previous_source = make_frame(1920, 1080);
	auto previous_composited = previous_source;
	fill_box(previous_composited, 720, 920, 420, 64, 220, 220, 220);

	auto current_source = make_frame(1920, 1080);
	fill_box(current_source, 720, 920, 420, 64, 16, 24, 40);
	auto current_composited = current_source;
	fill_box(current_composited, 720, 920, 420, 64, 228, 232, 240);

	return {
		std::move(previous_source),
		std::move(previous_composited),
		std::move(current_source),
		std::move(current_composited)
	};
}

TransitionCase make_motion_case() {
	auto previous_source = make_frame(1920, 1080);
	auto previous_composited = previous_source;
	fill_box(previous_composited, 120, 920, 180, 48, 255, 255, 255);
	fill_box(previous_composited, 1480, 920, 180, 48, 180, 220, 255);

	auto current_source = make_frame(1920, 1080);
	auto current_composited = current_source;
	fill_box(current_composited, 320, 920, 180, 48, 255, 255, 255);
	fill_box(current_composited, 1280, 920, 180, 48, 180, 220, 255);

	return {
		std::move(previous_source),
		std::move(previous_composited),
		std::move(current_source),
		std::move(current_composited)
	};
}

TransitionCase make_disappear_case() {
	auto previous_source = make_frame(1920, 1080);
	auto previous_composited = previous_source;
	fill_box(previous_composited, 640, 900, 260, 40, 255, 255, 255);
	fill_box(previous_composited, 980, 900, 300, 40, 180, 220, 255);

	auto current_source = make_frame(1920, 1080);
	auto current_composited = current_source;

	return {
		std::move(previous_source),
		std::move(previous_composited),
		std::move(current_source),
		std::move(current_composited)
	};
}

TransitionCase make_karaoke_case() {
	auto previous_source = make_frame(1920, 1080);
	auto previous_composited = previous_source;
	fill_box(previous_composited, 540, 920, 560, 56, 220, 220, 220);
	fill_box(previous_composited, 540, 920, 180, 56, 48, 196, 255);

	auto current_source = make_frame(1920, 1080);
	auto current_composited = current_source;
	fill_box(current_composited, 540, 920, 560, 56, 220, 220, 220);
	fill_box(current_composited, 540, 920, 320, 56, 48, 196, 255);

	return {
		std::move(previous_source),
		std::move(previous_composited),
		std::move(current_source),
		std::move(current_composited)
	};
}

TransitionCase make_banner_scroll_case() {
	auto previous_source = make_frame(1920, 1080);
	auto previous_composited = previous_source;
	fill_box(previous_composited, 80, 120, 1400, 18, 240, 240, 240);
	fill_box(previous_composited, 80, 138, 1400, 18, 120, 160, 220);

	auto current_source = make_frame(1920, 1080);
	auto current_composited = current_source;
	fill_box(current_composited, 144, 120, 1400, 18, 240, 240, 240);
	fill_box(current_composited, 144, 138, 1400, 18, 120, 160, 220);

	return {
		std::move(previous_source),
		std::move(previous_composited),
		std::move(current_source),
		std::move(current_composited)
	};
}

std::pair<std::uint64_t, std::pair<std::size_t, std::size_t>> bench_min_patch(VideoFrame const& source, VideoFrame const& composited) {
	SubtitleOverlayStorage storage;
	SubtitleOverlay overlay;
	auto ok = ExtractOpaqueBgraDifferenceOverlay(source, composited, storage, overlay);
	std::uint64_t checksum = ok ? static_cast<std::uint64_t>(overlay.width) * overlay.height : 0;
	return {checksum, {overlay_storage_bytes(storage), storage.pixels.size()}};
}

std::pair<std::uint64_t, std::pair<std::size_t, std::size_t>> bench_sparse_first(VideoFrame const& source, VideoFrame const& composited) {
	SubtitleOverlayStorage storage;
	SubtitleOverlay overlay;
	BuildSparsePremultipliedCompatibilityOverlay(source, composited, storage, overlay);
	BuildDirtyTileRectsForOverlay(nullptr, storage, 64, 64);
	overlay = storage.MakeView(true);
	std::uint64_t checksum = static_cast<std::uint64_t>(overlay.dirty_rect_count) + (storage.has_visible_content ? 1 : 0);
	return {checksum, {overlay_storage_bytes(storage), dirty_upload_bytes(overlay)}};
}

std::pair<std::uint64_t, std::pair<std::size_t, std::size_t>> bench_sparse_steady(VideoFrame const& source, VideoFrame const& composited, SubtitleOverlayStorage const& previous) {
	SubtitleOverlayStorage current;
	SubtitleOverlay overlay;
	BuildSparsePremultipliedCompatibilityOverlay(source, composited, current, overlay);
	BuildDirtyTileRectsForOverlay(&previous, current, 64, 64);
	overlay = current.MakeView(true);
	std::uint64_t checksum = static_cast<std::uint64_t>(overlay.dirty_rect_count) + (current.has_visible_content ? 1 : 0);
	return {checksum, {overlay_storage_bytes(current), dirty_upload_bytes(overlay)}};
}

std::pair<std::uint64_t, std::pair<std::size_t, std::size_t>> bench_sparse_transition(TransitionCase const& frames, SubtitleOverlayStorage const& previous) {
	SubtitleOverlayStorage current;
	SubtitleOverlay overlay;
	BuildSparsePremultipliedCompatibilityOverlay(frames.current_source, frames.current_composited, current, overlay);
	BuildDirtyTileRectsForOverlay(&previous, current, 64, 64);
	overlay = current.MakeView(true);
	std::uint64_t checksum = static_cast<std::uint64_t>(overlay.dirty_rect_count) + (current.has_visible_content ? 1 : 0);
	return {checksum, {overlay_storage_bytes(current), dirty_upload_bytes(overlay)}};
}

std::pair<std::uint64_t, std::pair<std::size_t, std::size_t>> bench_sparse_steady_reused(
	VideoFrame const& source,
	VideoFrame const& composited,
	SubtitleOverlayStorage const& previous,
	SubtitleOverlayStorage& current) {
	SubtitleOverlay overlay;
	BuildSparsePremultipliedCompatibilityOverlay(source, composited, current, overlay);
	BuildDirtyTileRectsForOverlay(&previous, current, 64, 64);
	overlay = current.MakeView(true);
	std::uint64_t checksum = static_cast<std::uint64_t>(overlay.dirty_rect_count) + (current.has_visible_content ? 1 : 0);
	return {checksum, {overlay_storage_bytes(current), dirty_upload_bytes(overlay)}};
}

std::pair<std::uint64_t, std::pair<std::size_t, std::size_t>> bench_sparse_transition_reused(
	TransitionCase const& frames,
	SubtitleOverlayStorage const& previous,
	SubtitleOverlayStorage& current) {
	SubtitleOverlay overlay;
	BuildSparsePremultipliedCompatibilityOverlay(frames.current_source, frames.current_composited, current, overlay);
	BuildDirtyTileRectsForOverlay(&previous, current, 64, 64);
	overlay = current.MakeView(true);
	std::uint64_t checksum = static_cast<std::uint64_t>(overlay.dirty_rect_count) + (current.has_visible_content ? 1 : 0);
	return {checksum, {overlay_storage_bytes(current), dirty_upload_bytes(overlay)}};
}

std::pair<std::uint64_t, std::pair<std::size_t, std::size_t>> bench_sparse_transition_fused(
	TransitionCase const& frames,
	SubtitleOverlayStorage const& previous,
	SubtitleOverlayStorage& current) {
	SubtitleOverlay overlay;
	BuildSparsePremultipliedCompatibilityOverlayWithDirtyTiles(
		frames.current_source,
		frames.current_composited,
		&previous,
		current,
		overlay,
		64,
		64);
	std::uint64_t checksum = static_cast<std::uint64_t>(overlay.dirty_rect_count) + (current.has_visible_content ? 1 : 0);
	return {checksum, {overlay_storage_bytes(current), dirty_upload_bytes(overlay)}};
}

SubtitleOverlayStorage prepare_previous_sparse_overlay(VideoFrame const& source, VideoFrame const& composited) {
	SubtitleOverlayStorage storage;
	SubtitleOverlay overlay;
	BuildSparsePremultipliedCompatibilityOverlay(source, composited, storage, overlay);
	BuildDirtyTileRectsForOverlay(nullptr, storage, 64, 64);
	return storage;
}

void print_transition_group(
	char const* title,
	TransitionCase const& frames,
	SubtitleOverlayStorage const& previous,
	SubtitleOverlayStorage& reusable) {
	print_group(title, {
		run_bench("min_patch_extract", 50, [&] { return bench_min_patch(frames.current_source, frames.current_composited); }),
		run_bench("sparse_surface_first", 50, [&] { return bench_sparse_first(frames.current_source, frames.current_composited); }),
		run_bench("sparse_surface_transition", 50, [&] { return bench_sparse_transition(frames, previous); }),
		run_bench("sparse_surface_reused", 50, [&] { return bench_sparse_transition_reused(frames, previous, reusable); }),
		run_bench("sparse_surface_fused", 50, [&] { return bench_sparse_transition_fused(frames, previous, reusable); }),
	});
}
}

int main() {
	auto single_patch = make_single_patch_case();
	auto sparse = make_sparse_case();
	auto background_change = make_background_change_case();
	auto motion = make_motion_case();
	auto disappear = make_disappear_case();
	auto karaoke = make_karaoke_case();
	auto banner_scroll = make_banner_scroll_case();
	auto previous_single_patch = prepare_previous_sparse_overlay(single_patch.first, single_patch.second);
	auto previous_sparse = prepare_previous_sparse_overlay(sparse.first, sparse.second);
	auto previous_background_change = prepare_previous_sparse_overlay(background_change.previous_source, background_change.previous_composited);
	auto previous_motion = prepare_previous_sparse_overlay(motion.previous_source, motion.previous_composited);
	auto previous_disappear = prepare_previous_sparse_overlay(disappear.previous_source, disappear.previous_composited);
	auto previous_karaoke = prepare_previous_sparse_overlay(karaoke.previous_source, karaoke.previous_composited);
	auto previous_banner_scroll = prepare_previous_sparse_overlay(banner_scroll.previous_source, banner_scroll.previous_composited);
	SubtitleOverlayStorage reusable_single_patch = previous_single_patch;
	SubtitleOverlayStorage reusable_sparse = previous_sparse;
	SubtitleOverlayStorage reusable_background_change = previous_background_change;
	SubtitleOverlayStorage reusable_motion = previous_motion;
	SubtitleOverlayStorage reusable_disappear = previous_disappear;
	SubtitleOverlayStorage reusable_karaoke = previous_karaoke;
	SubtitleOverlayStorage reusable_banner_scroll = previous_banner_scroll;

	std::cout << "CSRI overlay extraction benchmark\n";
	print_group("Single subtitle block", {
		run_bench("min_patch_extract", 50, [&] { return bench_min_patch(single_patch.first, single_patch.second); }),
		run_bench("sparse_surface_first", 50, [&] { return bench_sparse_first(single_patch.first, single_patch.second); }),
		run_bench("sparse_surface_steady", 50, [&] { return bench_sparse_steady(single_patch.first, single_patch.second, previous_single_patch); }),
		run_bench("sparse_surface_reused", 50, [&] { return bench_sparse_steady_reused(single_patch.first, single_patch.second, previous_single_patch, reusable_single_patch); }),
	});

	print_group("Sparse multi-region subtitles", {
		run_bench("min_patch_extract", 50, [&] { return bench_min_patch(sparse.first, sparse.second); }),
		run_bench("sparse_surface_first", 50, [&] { return bench_sparse_first(sparse.first, sparse.second); }),
		run_bench("sparse_surface_steady", 50, [&] { return bench_sparse_steady(sparse.first, sparse.second, previous_sparse); }),
		run_bench("sparse_surface_reused", 50, [&] { return bench_sparse_steady_reused(sparse.first, sparse.second, previous_sparse, reusable_sparse); }),
	});

	print_transition_group("Stable subtitle, changing video", background_change, previous_background_change, reusable_background_change);
	print_transition_group("Subtitle motion across tiles", motion, previous_motion, reusable_motion);
	print_transition_group("Subtitle disappears", disappear, previous_disappear, reusable_disappear);
	print_transition_group("Karaoke progression within line", karaoke, previous_karaoke, reusable_karaoke);
	print_transition_group("Scrolling banner across tiles", banner_scroll, previous_banner_scroll, reusable_banner_scroll);

	return 0;
}
