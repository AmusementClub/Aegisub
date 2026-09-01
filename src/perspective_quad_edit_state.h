#pragma once

#include "perspective_quad_geometry.h"

#include <optional>

namespace perspective {

enum class PerspectiveQuadHandle {
	None,
	TopLeft,
	TopRight,
	BottomRight,
	BottomLeft,
	Center,
	EdgeTop,    ///< midpoint of the TopLeft-TopRight edge
	EdgeRight,  ///< midpoint of the TopRight-BottomRight edge
	EdgeBottom, ///< midpoint of the BottomRight-BottomLeft edge
	EdgeLeft,   ///< midpoint of the BottomLeft-TopLeft edge
};

// Modifier semantics applied to an in-progress quad edit gesture. Both may be
// combined; center drags are unaffected by symmetric mirroring.
struct PerspectiveQuadGestureModifiers {
	/// Clamp the drag delta to its dominant axis.
	bool axis_locked = false;
	/// Mirror the drag delta onto the opposite corner or edge.
	bool symmetric = false;
};

enum class PerspectiveQuadEditError {
	None,
	NotBound,
	NoTarget,
	NoCurrentQuad,
	ReadOnly,
	InvalidSource,
	InvalidHandle,
	GestureAlreadyActive,
	NoGesture,
	InvalidTolerance,
	NonFinitePointer,
	CoordinateOutOfRange,
};

[[nodiscard]] char const* DescribePerspectiveQuadEditError(
	PerspectiveQuadEditError error);

// Unlike QuadCenter, this remains available for finite invalid quads so the
// translation handle does not disappear while the user repairs the target.
[[nodiscard]] std::optional<Vec2> PerspectiveQuadArithmeticCenter(
	Quad const& quad);

// Both the quad and point must be expressed in the same coordinate space.
// Corners have priority over edges, edges over the center; corners retain
// TL/TR/BR/BL index order.
[[nodiscard]] PerspectiveQuadHandle HitTestPerspectiveQuad(
	Quad const& quad,
	Vec2 point,
	double tolerance);

// Normalizes any opposite-corner drag to semantic TL/TR/BR/BL order.
[[nodiscard]] Quad PerspectiveQuadFromOppositeCorners(Vec2 first, Vec2 second);
// Cyclically assigns semantic corner indices so point 0 -> 1 follows the
// nearest cardinal direction. Exact diagonal ties prefer the horizontal axis.
[[nodiscard]] Quad PerspectiveQuadFromOppositeCorners(
	Vec2 first, Vec2 second, std::optional<Vec2> first_edge_direction);
// True when the drag escaped the click tolerance on either axis, so flat or
// narrow quads are not mistaken for mis-clicks.
[[nodiscard]] bool IsPerspectiveQuadCreationDrag(
	Vec2 first_canvas,
	Vec2 second_canvas,
	double tolerance);

class PerspectiveQuadEditState {
	std::optional<Quad> current_quad;
	std::optional<Quad> target;
	std::optional<GeometryValidation> validation;
	std::optional<Quad> gesture_start_target;
	std::optional<GeometryValidation> gesture_start_validation;
	Vec2 gesture_start_pointer;
	PerspectiveQuadHandle active_handle = PerspectiveQuadHandle::None;
	bool bound = false;
	bool editable = false;

	void ClearGesture();

public:
	[[nodiscard]] PerspectiveQuadEditError Bind(
		std::optional<Quad> current,
		bool is_editable,
		bool initialize_target = false);
	// Recaptures the current subtitle geometry without discarding an editable
	// target. A source which becomes read-only shows its new current quad instead.
	[[nodiscard]] PerspectiveQuadEditError RefreshCurrent(
		std::optional<Quad> current,
		bool is_editable);
	[[nodiscard]] PerspectiveQuadEditError ReplaceTarget(Quad const& replacement);
	// Discards an editable target while keeping the binding and current quad.
	[[nodiscard]] PerspectiveQuadEditError DropTarget();
	void Clear();

	[[nodiscard]] bool IsBound() const noexcept { return bound; }
	[[nodiscard]] bool HasTarget() const noexcept { return target.has_value(); }
	[[nodiscard]] bool CanInitializeFromCurrent() const noexcept {
		return IsEditable() && current_quad.has_value()
			&& (!target || IsModified());
	}
	[[nodiscard]] bool IsEditable() const noexcept {
		return IsBound() && editable;
	}
	[[nodiscard]] bool IsGestureActive() const noexcept {
		return gesture_start_target.has_value();
	}
	[[nodiscard]] bool IsModified() const noexcept;
	[[nodiscard]] PerspectiveQuadHandle ActiveHandle() const noexcept {
		return active_handle;
	}

	[[nodiscard]] std::optional<Quad> const& Current() const noexcept {
		return current_quad;
	}
	[[nodiscard]] std::optional<Quad> const& Target() const noexcept {
		return target;
	}
	[[nodiscard]] std::optional<GeometryValidation> const& Validation() const noexcept {
		return validation;
	}

	// Convenience overload for callers whose pointer tolerance is expressed in
	// the same space as Target(). UI callers may hit-test a canvas-space copy
	// and use the explicit-handle overload with a script-space pointer.
	[[nodiscard]] PerspectiveQuadEditError BeginGesture(
		Vec2 point,
		double tolerance);
	[[nodiscard]] PerspectiveQuadEditError BeginGesture(
		PerspectiveQuadHandle handle,
		Vec2 point);
	// Modifiers are read per update so keys can be toggled mid-gesture; the
	// delta is always recomputed from the gesture start, never accumulated.
	[[nodiscard]] PerspectiveQuadEditError UpdateGesture(
		Vec2 point,
		PerspectiveQuadGestureModifiers modifiers = {});
	[[nodiscard]] PerspectiveQuadEditError FinishGesture();
	[[nodiscard]] PerspectiveQuadEditError CancelGesture();
	[[nodiscard]] PerspectiveQuadEditError UseCurrent();
};

}
