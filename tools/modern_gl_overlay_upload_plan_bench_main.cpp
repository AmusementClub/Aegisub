#include "../src/modern_gl_overlay_upload_plan.h"
#include "../src/subtitle_overlay.h"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

namespace {
using clock_type = std::chrono::steady_clock;

struct BenchCase {
	std::string name;
	ModernGLOverlayLayerState initial_state;
	SubtitleOverlayStorage storage;
	SubtitleOverlay overlay = { };
};

std::size_t dirty_upload_bytes(SubtitleOverlay const& overlay) {
	if (!overlay.dirty_rects || overlay.dirty_rect_count <= 0)
		return 0;

	std::size_t total = 0;
	for (int i = 0; i < overlay.dirty_rect_count; ++i)
		total += static_cast<std::size_t>(overlay.dirty_rects[i].width) * overlay.dirty_rects[i].height * 4;
	return total;
}

std::size_t full_upload_bytes(SubtitleOverlay const& overlay) {
	return static_cast<std::size_t>(overlay.width) * overlay.height * 4;
}

BenchCase make_case(char const* name, int width, int height, bool hidden, bool with_dirty) {
	BenchCase bench_case;
	bench_case.name = name;
	bench_case.initial_state.width = width;
	bench_case.initial_state.height = height;
	bench_case.initial_state.canvas_width = width;
	bench_case.initial_state.canvas_height = height;
	bench_case.initial_state.has_allocated_resources = true;
	bench_case.initial_state.has_visible_content = !hidden;
	bench_case.initial_state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	bench_case.storage.Reset(width, height, false);
	if (with_dirty)
		bench_case.storage.dirty_rects.push_back({ width / 4, height / 2, width / 8, height / 16 });
	bench_case.overlay = bench_case.storage.MakeView(true);
	bench_case.overlay.canvas_width = width;
	bench_case.overlay.canvas_height = height;
	bench_case.overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;
	return bench_case;
}

ModernGLOverlayUploadPlan LegacyOverlayPlan(ModernGLOverlayLayerState state, SubtitleOverlay const* overlay) {
	if (!IsValidDirectRenderableOverlayForModernGL(overlay)) {
		state = { };
		return { ModernGLOverlayUploadAction::HideKeepResources, state };
	}

	bool layout_changed =
		state.width != overlay->width ||
		state.height != overlay->height ||
		state.flipped != overlay->flipped ||
		state.canvas_width != overlay->canvas_width ||
		state.canvas_height != overlay->canvas_height ||
		state.offset_x != overlay->target_x ||
		state.offset_y != overlay->target_y ||
		state.composition_mode != overlay->composition_mode ||
		!state.has_visible_content ||
		!state.has_allocated_resources;

	if (layout_changed) {
		state.width = overlay->width;
		state.height = overlay->height;
		state.canvas_width = overlay->canvas_width;
		state.canvas_height = overlay->canvas_height;
		state.offset_x = overlay->target_x;
		state.offset_y = overlay->target_y;
		state.flipped = overlay->flipped;
		state.composition_mode = overlay->composition_mode;
		state.has_allocated_resources = true;
		state.has_visible_content = true;
		return { ModernGLOverlayUploadAction::FullUpload, state };
	}

	if (overlay->dirty_rect_count > 0 && overlay->dirty_rects) {
		state.has_visible_content = true;
		return { ModernGLOverlayUploadAction::DirtyUpload, state };
	}

	state.has_visible_content = true;
	return { ModernGLOverlayUploadAction::ReuseExistingContent, state };
}

std::size_t upload_bytes_for_plan(ModernGLOverlayUploadPlan const& plan, SubtitleOverlay const& overlay) {
	switch (plan.action) {
	case ModernGLOverlayUploadAction::FullUpload:
		return full_upload_bytes(overlay);
	case ModernGLOverlayUploadAction::DirtyUpload:
		return dirty_upload_bytes(overlay);
	case ModernGLOverlayUploadAction::HideKeepResources:
	case ModernGLOverlayUploadAction::ReuseExistingContent:
	default:
		return 0;
	}
}

template<typename Func>
double bench_ns_per_op(Func&& func) {
	volatile std::uint64_t sink = 0;
	for (int i = 0; i < 8; ++i)
		sink += func();

	auto start = clock_type::now();
	for (int i = 0; i < 200000; ++i)
		sink += func();
	auto end = clock_type::now();

	(void)sink;
	return std::chrono::duration<double, std::nano>(end - start).count() / 200000.0;
}
}

int main() {
	auto hidden_same = make_case("hide_then_same_reappear", 1920, 1080, true, false);
	auto hidden_dirty = make_case("hide_then_dirty_reappear", 1920, 1080, true, true);
	auto visible_stable = make_case("visible_stable_noop", 1920, 1080, false, false);

	std::cout << "Modern GL overlay reactivation plan benchmark\n";
	std::cout << std::left << std::setw(26) << "scenario"
		<< std::right << std::setw(14) << "legacy ns"
		<< std::setw(14) << "new ns"
		<< std::setw(16) << "legacy upload"
		<< std::setw(16) << "new upload"
		<< "\n";

	for (auto const* bench_case : { &hidden_same, &hidden_dirty, &visible_stable }) {
		auto legacy = LegacyOverlayPlan(bench_case->initial_state, &bench_case->overlay);
		auto current = DecideModernGLOverlayUploadPlan(bench_case->initial_state, &bench_case->overlay);
		double legacy_ns = bench_ns_per_op([&] {
			return static_cast<std::uint64_t>(upload_bytes_for_plan(LegacyOverlayPlan(bench_case->initial_state, &bench_case->overlay), bench_case->overlay));
		});
		double current_ns = bench_ns_per_op([&] {
			return static_cast<std::uint64_t>(upload_bytes_for_plan(DecideModernGLOverlayUploadPlan(bench_case->initial_state, &bench_case->overlay), bench_case->overlay));
		});

		std::cout << std::left << std::setw(26) << bench_case->name
			<< std::right << std::setw(14) << std::fixed << std::setprecision(2) << legacy_ns
			<< std::setw(14) << std::fixed << std::setprecision(2) << current_ns
			<< std::setw(16) << upload_bytes_for_plan(legacy, bench_case->overlay)
			<< std::setw(16) << upload_bytes_for_plan(current, bench_case->overlay)
			<< "\n";
	}

	return 0;
}
