#pragma once

#include <cstdint>

namespace aegisub::skia::audio {

enum class Backend {
	Wx,
	Skia,
};

enum class SelectionReason {
	RuntimeDisabled,
	ContextUnavailable,
	DesktopGlTooOld,
	SoftwareLikeRenderer,
	SkiaAvailable,
};

struct Capabilities {
	bool context_available = false;
	int gl_major = 0;
	int gl_minor = 0;
	bool software_like_renderer = false;
};

struct Selection {
	Backend backend = Backend::Wx;
	SelectionReason reason = SelectionReason::RuntimeDisabled;
};

bool ParseRuntimeOptIn(char const *value);
bool IsDesktopGlAtLeast(int major, int minor, int required_major, int required_minor);
Selection SelectBackend(bool runtime_requested, Capabilities const& capabilities);

enum class Layer : uint32_t {
	None = 0,
	Content = 1u << 0,
	Style = 1u << 1,
	Marker = 1u << 2,
	Cursor = 1u << 3,
	Chrome = 1u << 4,
	All = (1u << 5) - 1,
};

constexpr Layer operator|(Layer lhs, Layer rhs) {
	return static_cast<Layer>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

constexpr Layer operator&(Layer lhs, Layer rhs) {
	return static_cast<Layer>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
}

constexpr bool HasLayer(Layer mask, Layer layer) {
	return (mask & layer) != Layer::None;
}

enum class Change {
	Provider,
	AnalysisSettings,
	Zoom,
	Scroll,
	Amplitude,
	Palette,
	Style,
	Marker,
	Cursor,
	Chrome,
	Resize,
	Dpi,
};

struct Revisions {
	uint64_t provider = 1;
	uint64_t analysis = 1;
	uint64_t viewport = 1;
	uint64_t content = 1;
	uint64_t style = 1;
	uint64_t marker = 1;
	uint64_t cursor = 1;
	uint64_t chrome = 1;

	friend bool operator==(Revisions const&, Revisions const&) = default;
};

struct TransitionPlan {
	Revisions next;
	Layer dirty_layers = Layer::None;
	bool invalidate_analysis_tiles = false;
	bool invalidate_gpu_content_tiles = false;
	bool invalidate_text_cache = false;
	bool request_visible_tiles = false;
	bool recreate_surface = false;
};

TransitionPlan PlanTransition(Revisions const& current, Change change);

enum class Failure {
	None,
	ContextInitialization,
	FrameBegin,
	Allocation,
	FlushSubmit,
	ContextLost,
};

class FailureState {
	Failure first_failure = Failure::None;

public:
	bool IsHealthy() const { return first_failure == Failure::None; }
	bool CanUseSkia() const { return IsHealthy(); }
	Failure FirstFailure() const { return first_failure; }
	bool MarkFailed(Failure failure);
};

}
