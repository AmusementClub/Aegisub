#include "../src/subtitle_overlay_blend.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
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

void reference_blend_libass_mask_into_bgra_target(
	BgraSubtitleTargetView target,
	SubtitleOverlayBlendMode mode,
	int dst_x,
	int dst_y,
	int mask_w,
	int mask_h,
	unsigned char const* mask_data,
	ptrdiff_t mask_stride,
	std::uint32_t ass_color) {
	unsigned int opacity = 255 - (ass_color & 0xFFu);
	unsigned int r = ass_color >> 24;
	unsigned int g = (ass_color >> 16) & 0xFFu;
	unsigned int b = (ass_color >> 8) & 0xFFu;

	int x0 = std::max(0, dst_x);
	int y0 = std::max(0, dst_y);
	int x1 = std::min(target.width, dst_x + mask_w);
	int y1 = std::min(target.height, dst_y + mask_h);
	if (x0 >= x1 || y0 >= y1)
		return;

	for (int y = y0; y < y1; ++y) {
		auto* dst_row = target.data + static_cast<std::ptrdiff_t>(y) * target.stride;
		auto const* src_row = mask_data + static_cast<std::ptrdiff_t>(y - dst_y) * mask_stride;
		for (int x = x0; x < x1; ++x) {
			unsigned int src_alpha = static_cast<unsigned int>(src_row[x - dst_x]) * opacity / 255;
			if (!src_alpha)
				continue;

			auto* dst = dst_row + static_cast<std::ptrdiff_t>(x) * 4;
			if (mode == SubtitleOverlayBlendMode::LegacyBakeIn) {
				unsigned int inv_alpha = 255 - src_alpha;
				dst[0] = static_cast<unsigned char>((src_alpha * b + inv_alpha * dst[0]) / 255);
				dst[1] = static_cast<unsigned char>((src_alpha * g + inv_alpha * dst[1]) / 255);
				dst[2] = static_cast<unsigned char>((src_alpha * r + inv_alpha * dst[2]) / 255);
				dst[3] = 0;
			}
			else {
				unsigned int inv_alpha = 255 - src_alpha;
				unsigned int src_b = src_alpha * b / 255;
				unsigned int src_g = src_alpha * g / 255;
				unsigned int src_r = src_alpha * r / 255;
				dst[0] = static_cast<unsigned char>(src_b + dst[0] * inv_alpha / 255);
				dst[1] = static_cast<unsigned char>(src_g + dst[1] * inv_alpha / 255);
				dst[2] = static_cast<unsigned char>(src_r + dst[2] * inv_alpha / 255);
				dst[3] = static_cast<unsigned char>(src_alpha + dst[3] * inv_alpha / 255);
			}
		}
	}
}

void reference_composite_premultiplied_overlay(VideoFrame& frame, SubtitleOverlay const& overlay) {
	int x0 = std::max(0, overlay.target_x);
	int y0 = std::max(0, overlay.target_y);
	int x1 = std::min(static_cast<int>(frame.width), overlay.target_x + overlay.width);
	int y1 = std::min(static_cast<int>(frame.height), overlay.target_y + overlay.height);
	if (x0 >= x1 || y0 >= y1)
		return;

	for (int y = y0; y < y1; ++y) {
		int overlay_y = y - overlay.target_y;
		auto* dst_row = frame.data.data() + static_cast<std::ptrdiff_t>(y) * frame.pitch;
		auto const* src_row = overlay.planes[0].data + static_cast<std::ptrdiff_t>(overlay_y) * overlay.planes[0].stride;
		for (int x = x0; x < x1; ++x) {
			auto const* src = src_row + static_cast<std::ptrdiff_t>(x - overlay.target_x) * 4;
			auto* dst = dst_row + static_cast<std::ptrdiff_t>(x) * 4;
			unsigned int src_alpha = src[3];
			if (!src_alpha)
				continue;

			unsigned int inv_alpha = 255 - src_alpha;
			dst[0] = static_cast<unsigned char>(src[0] + dst[0] * inv_alpha / 255);
			dst[1] = static_cast<unsigned char>(src[1] + dst[1] * inv_alpha / 255);
			dst[2] = static_cast<unsigned char>(src[2] + dst[2] * inv_alpha / 255);
			dst[3] = 0;
		}
	}
}

std::vector<unsigned char> make_bgra_pixels(int width, int height, bool with_alpha) {
	std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			auto* pixel = pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
			pixel[0] = static_cast<unsigned char>((x * 13 + y * 7) & 0xFF);
			pixel[1] = static_cast<unsigned char>((x * 5 + y * 17) & 0xFF);
			pixel[2] = static_cast<unsigned char>((x * 19 + y * 3) & 0xFF);
			pixel[3] = with_alpha ? static_cast<unsigned char>((x * 11 + y * 23) & 0xFF) : 0;
		}
	}
	return pixels;
}

std::vector<unsigned char> make_mask_pixels(int width, int height) {
	std::vector<unsigned char> mask(static_cast<std::size_t>(width) * height);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			mask[static_cast<std::size_t>(y) * width + x] =
				static_cast<unsigned char>(((x * 29) ^ (y * 17) ^ ((x + y) * 7)) & 0xFF);
		}
	}
	return mask;
}

std::uint64_t checksum_bytes(std::vector<unsigned char> const& bytes) {
	return std::accumulate(bytes.begin(), bytes.end(), std::uint64_t{0});
}

std::uint64_t checksum_frame(VideoFrame const& frame) {
	return checksum_bytes(frame.data);
}

std::pair<std::uint64_t, std::pair<std::size_t, std::size_t>> bench_blend_mask(bool reference, SubtitleOverlayBlendMode mode) {
	constexpr int target_width = 512;
	constexpr int target_height = 128;
	constexpr int mask_width = 384;
	constexpr int mask_height = 96;
	constexpr int dst_x = 48;
	constexpr int dst_y = 16;
	auto base = make_bgra_pixels(target_width, target_height, mode == SubtitleOverlayBlendMode::PremultipliedOverlay);
	auto mask = make_mask_pixels(mask_width, mask_height);
	auto working = base;

	BgraSubtitleTargetView target {
		working.data(),
		target_width * 4,
		target_width,
		target_height,
		false
	};

	if (reference) {
		reference_blend_libass_mask_into_bgra_target(
			target, mode, dst_x, dst_y, mask_width, mask_height, mask.data(), mask_width, 0x2E84D000u);
	}
	else {
		BlendLibassMaskIntoBgraTarget(
			target, mode, dst_x, dst_y, mask_width, mask_height, mask.data(), mask_width, 0x2E84D000u);
	}

	return {
		checksum_bytes(working),
		{ working.size(), static_cast<std::size_t>(mask_width) * mask_height }
	};
}

std::pair<std::uint64_t, std::pair<std::size_t, std::size_t>> bench_composite_overlay(bool reference) {
	constexpr int frame_width = 512;
	constexpr int frame_height = 128;
	constexpr int overlay_width = 384;
	constexpr int overlay_height = 96;
	constexpr int target_x = 48;
	constexpr int target_y = 16;
	VideoFrame frame = make_frame(frame_width, frame_height);
	frame.data = make_bgra_pixels(frame_width, frame_height, false);

	SubtitleOverlayStorage storage;
	storage.Reset(overlay_width, overlay_height, false);
	for (int y = 0; y < overlay_height; ++y) {
		for (int x = 0; x < overlay_width; ++x) {
			auto* pixel = storage.pixels.data() + (static_cast<std::size_t>(y) * overlay_width + x) * 4;
			unsigned int alpha = static_cast<unsigned int>(((x * 31) + (y * 19) + 53) & 0xFF);
			pixel[0] = static_cast<unsigned char>(alpha * 48 / 255);
			pixel[1] = static_cast<unsigned char>(alpha * 180 / 255);
			pixel[2] = static_cast<unsigned char>(alpha * 250 / 255);
			pixel[3] = static_cast<unsigned char>(alpha);
		}
	}
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = frame_width;
	overlay.canvas_height = frame_height;
	overlay.target_x = target_x;
	overlay.target_y = target_y;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	if (reference)
		reference_composite_premultiplied_overlay(frame, overlay);
	else
		CompositePremultipliedBgraOverlayOntoVideoFrame(frame, overlay);

	return {
		checksum_frame(frame),
		{ frame.data.size(), storage.pixels.size() }
	};
}

void ensure_blend_parity() {
	auto legacy_reference = bench_blend_mask(true, SubtitleOverlayBlendMode::LegacyBakeIn);
	auto legacy_actual = bench_blend_mask(false, SubtitleOverlayBlendMode::LegacyBakeIn);
	if (legacy_reference.first != legacy_actual.first)
		throw std::runtime_error("Legacy alpha blend checksum mismatch.");

	auto premul_reference = bench_blend_mask(true, SubtitleOverlayBlendMode::PremultipliedOverlay);
	auto premul_actual = bench_blend_mask(false, SubtitleOverlayBlendMode::PremultipliedOverlay);
	if (premul_reference.first != premul_actual.first)
		throw std::runtime_error("Premultiplied alpha blend checksum mismatch.");
}

void ensure_composite_parity() {
	auto reference = bench_composite_overlay(true);
	auto actual = bench_composite_overlay(false);
	if (reference.first != actual.first)
		throw std::runtime_error("Overlay composite checksum mismatch.");
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

	ensure_blend_parity();
	ensure_composite_parity();
	print_group("Alpha and composite kernels", {
		run_bench("blend_legacy_reference", 200, [&] { return bench_blend_mask(true, SubtitleOverlayBlendMode::LegacyBakeIn); }),
		run_bench("blend_legacy_fast", 200, [&] { return bench_blend_mask(false, SubtitleOverlayBlendMode::LegacyBakeIn); }),
		run_bench("blend_premul_reference", 200, [&] { return bench_blend_mask(true, SubtitleOverlayBlendMode::PremultipliedOverlay); }),
		run_bench("blend_premul_fast", 200, [&] { return bench_blend_mask(false, SubtitleOverlayBlendMode::PremultipliedOverlay); }),
		run_bench("composite_reference", 200, [&] { return bench_composite_overlay(true); }),
		run_bench("composite_fast", 200, [&] { return bench_composite_overlay(false); }),
	});

	return 0;
}
