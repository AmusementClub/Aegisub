#include "skia_audio_display_contract.h"

#include <cctype>
#include <limits>

namespace aegisub::skia::audio {
namespace {

uint64_t NextRevision(uint64_t value) {
	return value == std::numeric_limits<uint64_t>::max() ? value : value + 1;
}

void Bump(uint64_t& revision) {
	revision = NextRevision(revision);
}

}

bool ParseRuntimeOptIn(char const *value) {
	if (!value || !*value)
		return false;

	auto const first = static_cast<char>(std::tolower(static_cast<unsigned char>(*value)));
	return first != '0' && first != 'f' && first != 'n';
}

bool IsDesktopGlAtLeast(int major, int minor, int required_major, int required_minor) {
	if (major != required_major)
		return major > required_major;
	return minor >= required_minor;
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
			Bump(plan.next.content);
			Bump(plan.next.style);
			Bump(plan.next.marker);
			Bump(plan.next.cursor);
			Bump(plan.next.chrome);
			plan.dirty_layers = Layer::All;
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
