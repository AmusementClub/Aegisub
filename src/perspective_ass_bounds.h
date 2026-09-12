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
	// Ratio of the layout's horizontal and vertical pixels per script unit.
	// Text glyph size follows the vertical ratio; spacing follows the horizontal.
	double layout_aspect = 1.0;
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
	// Face the text measurement rejected, when error is FontUnavailable, so a
	// diagnostic can name the font instead of refusing generically. Empty for
	// every other error.
	std::string font_name;

	explicit operator bool() const { return error == AssBoundsError::None; }
};

[[nodiscard]] char const* DescribeAssBoundsError(AssBoundsError error);
[[nodiscard]] AssBoundsResult EvaluateAssBaseBounds(AssBoundsInput const& input);

}
