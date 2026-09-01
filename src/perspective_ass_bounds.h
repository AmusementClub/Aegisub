#pragma once

#include "perspective_forward.h"

#include <string>

class AssDialogue;
class AssStyle;

namespace perspective {

struct EffectiveAssState;

using AssTextExtentsProvider = bool (*)(
	AssStyle*, std::string const&, double&, double&, double&, double&);

// The GUI host injects the platform text extent service exposed to Automation;
// headless callers and tests can omit it for a deterministic approximation.
struct AssBoundsInput {
	AssDialogue const* line = nullptr;
	EffectiveAssState const* state = nullptr;
	AssTextExtentsProvider text_extents = nullptr;
};

enum class AssBoundsError {
	None,
	InvalidInput,
	EmptyGeometry,
	FontUnavailable,
	UnsupportedAutomaticWrap,
	MixedGeometryRuns,
	UnsupportedDrawingLayout,
	UnsupportedDrawingCommand,
	DrawingStateMismatch,
	InvalidDrawingSyntax,
	InvalidDrawingGeometry,
	InvalidDrawingBounds,
};

struct AssBoundsResult {
	AssBoundsError error = AssBoundsError::None;
	GeometryError geometry_error = GeometryError::None;
	BaseBounds value;

	explicit operator bool() const { return error == AssBoundsError::None; }
};

[[nodiscard]] char const* DescribeAssBoundsError(AssBoundsError error);
[[nodiscard]] AssBoundsResult EvaluateAssBaseBounds(AssBoundsInput const& input);

}
