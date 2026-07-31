#include "skia_audio_display_contract.h"

#include <charconv>
#include <cctype>
#include <limits>
#include <string>

namespace aegisub::skia::audio {
namespace {

uint64_t NextRevision(uint64_t value) {
	return value == std::numeric_limits<uint64_t>::max() ? value : value + 1;
}

void Bump(uint64_t& revision) {
	revision = NextRevision(revision);
}

}

bool ShouldCreateSkiaWidget(bool runtime_requested, bool presenter_available) {
	return runtime_requested && presenter_available;
}

bool IsSoftwareLikeGlRenderer(std::string_view vendor, std::string_view renderer) {
	std::string value;
	value.reserve(vendor.size() + renderer.size() + 1);
	value.append(vendor);
	value.push_back(' ');
	value.append(renderer);
	for (char& ch : value)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

	for (auto const token : {
		"gdi generic",
		"microsoft basic render driver",
		"llvmpipe",
		"softpipe",
		"software rasterizer",
		"swiftshader",
	}) {
		if (value.find(token) != std::string::npos)
			return true;
	}
	return false;
}

bool IsDesktopGlAtLeast(int major, int minor, int required_major, int required_minor) {
	if (major != required_major)
		return major > required_major;
	return minor >= required_minor;
}

FailureInjection ParseFailureInjection(std::string_view value) noexcept {
	if (value.empty() || value == "none")
		return FailureInjection::None;
	if (value == "context-init")
		return FailureInjection::ContextInitialization;
	if (value == "frame-begin")
		return FailureInjection::FrameBegin;
	if (value == "flush-submit")
		return FailureInjection::FlushSubmit;
	return FailureInjection::Unsupported;
}

std::uint64_t ParseFailureInjectionAfterContentFrames(std::string_view value) noexcept {
	if (value.empty())
		return 0;
	std::uint64_t frames = 0;
	auto const result = std::from_chars(value.data(), value.data() + value.size(), frames);
	return result.ec == std::errc{}
		&& result.ptr == value.data() + value.size()
		&& frames > 0
		? frames
		: 0;
}

FrameTargetValidation ValidateFrameTarget(FrameTarget const& target, std::uint64_t context_generation) {
	if (!context_generation)
		return { false, "the context generation is zero" };
	if (target.context_generation != context_generation)
		return { false, "the frame target context generation does not match the device token" };
	if (target.width <= 0 || target.height <= 0)
		return { false, "the frame target dimensions are not positive" };
	if (target.sample_count < 0 || target.stencil_bits < 0)
		return { false, "the frame target sample or stencil count is negative" };
	return { true, {} };
}

SurfaceKey MakeSurfaceKey(FrameTarget const& target) {
	return {
		target.context_generation,
		target.width,
		target.height,
		target.sample_count,
		target.stencil_bits,
		target.framebuffer_id,
		target.bottom_left_origin,
	};
}

Selection SelectBackend(bool runtime_requested, Capabilities const& capabilities) {
	if (!runtime_requested)
		return { Backend::Wx, SelectionReason::RuntimeDisabled };
	if (!capabilities.context_available)
		return { Backend::Wx, SelectionReason::ContextUnavailable };
	if (!IsDesktopGlAtLeast(capabilities.gl_major, capabilities.gl_minor, 2, 0))
		return { Backend::Wx, SelectionReason::DesktopGlTooOld };
	if (capabilities.software_like_renderer)
		return { Backend::Wx, SelectionReason::SoftwareLikeRenderer };
	return { Backend::Skia, SelectionReason::SkiaAvailable };
}

RuntimeFallbackDisposition PlanRuntimeFallback(bool content_frame_presented) noexcept {
	return content_frame_presented
		? RuntimeFallbackDisposition::Confirm
		: RuntimeFallbackDisposition::Automatic;
}

bool ShouldRetainLastCompleteContentFrame(
	bool content_viewport_complete,
	bool scrollbar_dragging) noexcept {
	return scrollbar_dragging && !content_viewport_complete;
}

TransitionPlan PlanTransition(Revisions const& current, Change change) {
	TransitionPlan plan;
	plan.next = current;

	switch (change) {
		case Change::Provider:
			Bump(plan.next.provider);
			Bump(plan.next.analysis);
			Bump(plan.next.viewport);
			Bump(plan.next.content);
			Bump(plan.next.style);
			Bump(plan.next.marker);
			Bump(plan.next.cursor);
			Bump(plan.next.chrome);
			plan.dirty_layers = Layer::All;
			plan.invalidate_analysis_tiles = true;
			plan.invalidate_gpu_content_tiles = true;
			plan.invalidate_text_cache = true;
			plan.request_visible_tiles = true;
			break;

		case Change::ContentReady:
			Bump(plan.next.content);
			plan.dirty_layers = Layer::Content;
			break;

		case Change::AnalysisSettings:
			Bump(plan.next.analysis);
			Bump(plan.next.content);
			plan.dirty_layers = Layer::Content;
			plan.invalidate_analysis_tiles = true;
			plan.invalidate_gpu_content_tiles = true;
			plan.request_visible_tiles = true;
			break;

		case Change::Zoom:
			Bump(plan.next.analysis);
			Bump(plan.next.viewport);
			Bump(plan.next.content);
			Bump(plan.next.chrome);
			plan.dirty_layers = Layer::All;
			plan.invalidate_analysis_tiles = true;
			plan.invalidate_gpu_content_tiles = true;
			plan.invalidate_text_cache = true;
			plan.request_visible_tiles = true;
			break;

		case Change::Scroll:
			Bump(plan.next.viewport);
			plan.dirty_layers = Layer::All;
			plan.request_visible_tiles = true;
			break;

		case Change::Amplitude:
			Bump(plan.next.content);
			plan.dirty_layers = Layer::Content;
			break;

		case Change::Palette:
			Bump(plan.next.content);
			Bump(plan.next.style);
			plan.dirty_layers = Layer::Content | Layer::Style;
			break;

		case Change::Style:
			Bump(plan.next.style);
			plan.dirty_layers = Layer::Style;
			break;

		case Change::Marker:
			Bump(plan.next.marker);
			plan.dirty_layers = Layer::Marker;
			break;

		case Change::Cursor:
			Bump(plan.next.cursor);
			plan.dirty_layers = Layer::Cursor;
			break;

		case Change::Chrome:
			Bump(plan.next.chrome);
			plan.dirty_layers = Layer::Chrome;
			plan.invalidate_text_cache = true;
			break;

		case Change::Resize:
			Bump(plan.next.viewport);
			Bump(plan.next.content);
			Bump(plan.next.chrome);
			plan.dirty_layers = Layer::All;
			plan.request_visible_tiles = true;
			plan.recreate_surface = true;
			break;

		case Change::Dpi:
			Bump(plan.next.viewport);
			Bump(plan.next.analysis);
			Bump(plan.next.content);
			Bump(plan.next.style);
			Bump(plan.next.marker);
			Bump(plan.next.cursor);
			Bump(plan.next.chrome);
			plan.dirty_layers = Layer::All;
			plan.invalidate_analysis_tiles = true;
			plan.invalidate_gpu_content_tiles = true;
			plan.invalidate_text_cache = true;
			plan.request_visible_tiles = true;
			plan.recreate_surface = true;
			break;
	}

	return plan;
}

bool FailureState::MarkFailed(Failure failure) {
	if (failure == Failure::None || first_failure != Failure::None)
		return false;
	first_failure = failure;
	return true;
}

}
