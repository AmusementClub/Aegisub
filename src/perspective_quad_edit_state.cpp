#include "perspective_quad_edit_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

namespace perspective {
namespace {

bool IsFinite(Vec2 point) {
	return std::isfinite(point.x) && std::isfinite(point.y);
}

bool IsInSafeRange(Vec2 point) {
	return std::abs(point.x) <= MaxAbsCoordinate
		&& std::abs(point.y) <= MaxAbsCoordinate;
}

bool SameQuad(Quad const& left, Quad const& right) {
	for (std::size_t index = 0; index < left.size(); ++index) {
		if (left[index].x != right[index].x || left[index].y != right[index].y)
			return false;
	}
	return true;
}

std::optional<std::size_t> CornerIndex(PerspectiveQuadHandle handle) {
	switch (handle) {
		case PerspectiveQuadHandle::TopLeft: return 0;
		case PerspectiveQuadHandle::TopRight: return 1;
		case PerspectiveQuadHandle::BottomRight: return 2;
		case PerspectiveQuadHandle::BottomLeft: return 3;
		case PerspectiveQuadHandle::None:
		case PerspectiveQuadHandle::Center:
			return std::nullopt;
	}
	return std::nullopt;
}

bool IsEditableHandle(PerspectiveQuadHandle handle) {
	switch (handle) {
		case PerspectiveQuadHandle::TopLeft:
		case PerspectiveQuadHandle::TopRight:
		case PerspectiveQuadHandle::BottomRight:
		case PerspectiveQuadHandle::BottomLeft:
		case PerspectiveQuadHandle::Center:
		case PerspectiveQuadHandle::EdgeTop:
		case PerspectiveQuadHandle::EdgeRight:
		case PerspectiveQuadHandle::EdgeBottom:
		case PerspectiveQuadHandle::EdgeLeft:
			return true;
		case PerspectiveQuadHandle::None:
			return false;
	}
	return false;
}

// The two corner indices an edge handle moves together, in TL/TR/BR/BL order.
std::optional<std::array<std::size_t, 2>> EdgeCornerIndices(
	PerspectiveQuadHandle handle) {
	switch (handle) {
		case PerspectiveQuadHandle::EdgeTop:
			return std::array<std::size_t, 2> {0, 1};
		case PerspectiveQuadHandle::EdgeRight:
			return std::array<std::size_t, 2> {1, 2};
		case PerspectiveQuadHandle::EdgeBottom:
			return std::array<std::size_t, 2> {2, 3};
		case PerspectiveQuadHandle::EdgeLeft:
			return std::array<std::size_t, 2> {3, 0};
		case PerspectiveQuadHandle::None:
		case PerspectiveQuadHandle::TopLeft:
		case PerspectiveQuadHandle::TopRight:
		case PerspectiveQuadHandle::BottomRight:
		case PerspectiveQuadHandle::BottomLeft:
		case PerspectiveQuadHandle::Center:
			return std::nullopt;
	}
	return std::nullopt;
}

std::array<std::size_t, 2> OppositeEdgeCornerIndices(PerspectiveQuadHandle handle) {
	switch (handle) {
		case PerspectiveQuadHandle::EdgeTop: return {2, 3};
		case PerspectiveQuadHandle::EdgeRight: return {3, 0};
		case PerspectiveQuadHandle::EdgeBottom: return {0, 1};
		case PerspectiveQuadHandle::EdgeLeft: return {1, 2};
		case PerspectiveQuadHandle::None:
		case PerspectiveQuadHandle::TopLeft:
		case PerspectiveQuadHandle::TopRight:
		case PerspectiveQuadHandle::BottomRight:
		case PerspectiveQuadHandle::BottomLeft:
		case PerspectiveQuadHandle::Center:
			break;
	}
	return {0, 0};
}

std::optional<Vec2> EdgeMidpoint(Quad const& quad, PerspectiveQuadHandle handle) {
	auto const corners = EdgeCornerIndices(handle);
	if (!corners || corners->back() >= quad.size() || corners->front() >= quad.size())
		return std::nullopt;
	Vec2 const& first = quad[corners->front()];
	Vec2 const& second = quad[corners->back()];
	Vec2 const midpoint {(first.x + second.x) / 2.0, (first.y + second.y) / 2.0};
	if (!IsFinite(midpoint))
		return std::nullopt;
	return midpoint;
}

PerspectiveQuadEditError ValidatePointer(Vec2 point) {
	if (!IsFinite(point))
		return PerspectiveQuadEditError::NonFinitePointer;
	if (!IsInSafeRange(point))
		return PerspectiveQuadEditError::CoordinateOutOfRange;
	return PerspectiveQuadEditError::None;
}

}

char const* DescribePerspectiveQuadEditError(PerspectiveQuadEditError error) {
	switch (error) {
		case PerspectiveQuadEditError::None: return "valid quad edit operation";
		case PerspectiveQuadEditError::NotBound: return "Perspective edit state is not bound";
		case PerspectiveQuadEditError::NoTarget: return "no Perspective target quad exists";
		case PerspectiveQuadEditError::NoCurrentQuad: return "the current subtitle quad is unavailable";
		case PerspectiveQuadEditError::ReadOnly: return "the Perspective quad is read-only";
		case PerspectiveQuadEditError::InvalidSource: return "the current subtitle quad is invalid";
		case PerspectiveQuadEditError::InvalidHandle: return "no editable quad handle was selected";
		case PerspectiveQuadEditError::GestureAlreadyActive: return "a quad edit gesture is already active";
		case PerspectiveQuadEditError::NoGesture: return "no quad edit gesture is active";
		case PerspectiveQuadEditError::InvalidTolerance: return "quad hit tolerance must be finite and non-negative";
		case PerspectiveQuadEditError::NonFinitePointer: return "quad edit pointer is not finite";
		case PerspectiveQuadEditError::CoordinateOutOfRange: return "quad edit coordinate exceeds the supported range";
	}
	return "unknown quad edit error";
}

std::optional<Vec2> PerspectiveQuadArithmeticCenter(Quad const& quad) {
	Vec2 center;
	for (auto const point : quad) {
		if (!IsFinite(point))
			return std::nullopt;
		// Divide before adding so large finite display coordinates do not
		// overflow merely while locating the transient center handle.
		center.x += point.x / static_cast<double>(quad.size());
		center.y += point.y / static_cast<double>(quad.size());
	}
	if (!IsFinite(center))
		return std::nullopt;
	return center;
}

PerspectiveQuadHandle HitTestPerspectiveQuad(
	Quad const& quad,
	Vec2 point,
	double tolerance) {
	if (!IsFinite(point) || !std::isfinite(tolerance) || tolerance < 0.0)
		return PerspectiveQuadHandle::None;

	constexpr PerspectiveQuadHandle corner_handles[] = {
		PerspectiveQuadHandle::TopLeft,
		PerspectiveQuadHandle::TopRight,
		PerspectiveQuadHandle::BottomRight,
		PerspectiveQuadHandle::BottomLeft,
	};
	for (std::size_t index = 0; index < quad.size(); ++index) {
		if (!IsFinite(quad[index]))
			return PerspectiveQuadHandle::None;
		if (std::hypot(quad[index].x - point.x, quad[index].y - point.y)
			<= tolerance)
			return corner_handles[index];
	}

	constexpr PerspectiveQuadHandle edge_handles[] = {
		PerspectiveQuadHandle::EdgeTop,
		PerspectiveQuadHandle::EdgeRight,
		PerspectiveQuadHandle::EdgeBottom,
		PerspectiveQuadHandle::EdgeLeft,
	};
	for (auto const edge_handle : edge_handles) {
		auto const midpoint = EdgeMidpoint(quad, edge_handle);
		if (midpoint
			&& std::hypot(midpoint->x - point.x, midpoint->y - point.y)
				<= tolerance)
			return edge_handle;
	}

	auto const center = PerspectiveQuadArithmeticCenter(quad);
	if (center && std::hypot(center->x - point.x, center->y - point.y)
		<= tolerance)
		return PerspectiveQuadHandle::Center;
	return PerspectiveQuadHandle::None;
}

Quad PerspectiveQuadFromOppositeCorners(Vec2 first, Vec2 second) {
	double const left = std::min(first.x, second.x);
	double const right = std::max(first.x, second.x);
	double const top = std::min(first.y, second.y);
	double const bottom = std::max(first.y, second.y);
	return {{{left, top}, {right, top}, {right, bottom}, {left, bottom}}};
}

Quad PerspectiveQuadFromOppositeCorners(
	Vec2 first, Vec2 second, std::optional<Vec2> first_edge_direction) {
	auto result = PerspectiveQuadFromOppositeCorners(first, second);
	if (!first_edge_direction)
		return result;

	Vec2 const direction = *first_edge_direction;
	if (!std::isfinite(direction.x) || !std::isfinite(direction.y)
		|| std::max(std::abs(direction.x), std::abs(direction.y)) <= 1.0e-12)
		return result;

	std::size_t const best_rotation = std::abs(direction.x) >= std::abs(direction.y)
		? (direction.x >= 0.0 ? 0u : 2u)
		: (direction.y >= 0.0 ? 1u : 3u);

	Quad oriented;
	for (std::size_t index = 0; index < oriented.size(); ++index)
		oriented[index] = result[(best_rotation + index) % result.size()];
	return oriented;
}

bool IsPerspectiveQuadCreationDrag(
	Vec2 first_canvas,
	Vec2 second_canvas,
	double tolerance) {
	return IsFinite(first_canvas) && IsFinite(second_canvas)
		&& std::isfinite(tolerance) && tolerance >= 0.0
		&& (std::abs(second_canvas.x - first_canvas.x) > tolerance
			|| std::abs(second_canvas.y - first_canvas.y) > tolerance);
}

PerspectiveQuadEditError PerspectiveQuadEditState::Bind(
	std::optional<Quad> current,
	bool is_editable,
	bool initialize_target) {
	if (current) {
		auto const current_validation = ValidateQuad(*current);
		if (!current_validation) {
			Clear();
			return PerspectiveQuadEditError::InvalidSource;
		}
	}
	if (initialize_target && !current) {
		Clear();
		return PerspectiveQuadEditError::NoCurrentQuad;
	}

	current_quad = std::move(current);
	target = initialize_target ? current_quad : std::nullopt;
	validation = target
		? std::optional<GeometryValidation>(ValidateQuad(*target))
		: std::nullopt;
	bound = true;
	editable = is_editable;
	ClearGesture();
	return PerspectiveQuadEditError::None;
}

PerspectiveQuadEditError PerspectiveQuadEditState::RefreshCurrent(
	std::optional<Quad> current,
	bool is_editable) {
	if (IsGestureActive())
		return PerspectiveQuadEditError::GestureAlreadyActive;

	bool const preserve_target = IsEditable() && target.has_value() && is_editable;
	bool const initialize_target = !is_editable && current.has_value();
	auto previous_target = target;
	auto previous_validation = validation;

	auto const error = Bind(
		std::move(current),
		is_editable,
		initialize_target);
	if (error != PerspectiveQuadEditError::None || !preserve_target)
		return error;

	target = std::move(previous_target);
	validation = std::move(previous_validation);
	return PerspectiveQuadEditError::None;
}

PerspectiveQuadEditError PerspectiveQuadEditState::ReplaceTarget(
	Quad const& replacement) {
	if (!bound)
		return PerspectiveQuadEditError::NotBound;
	if (!editable)
		return PerspectiveQuadEditError::ReadOnly;
	if (IsGestureActive())
		return PerspectiveQuadEditError::GestureAlreadyActive;
	for (auto const point : replacement) {
		if (auto const pointer_error = ValidatePointer(point);
			pointer_error != PerspectiveQuadEditError::None)
			return pointer_error;
	}
	target = replacement;
	validation = ValidateQuad(replacement);
	ClearGesture();
	return PerspectiveQuadEditError::None;
}

PerspectiveQuadEditError PerspectiveQuadEditState::DropTarget() {
	if (!bound)
		return PerspectiveQuadEditError::NotBound;
	if (!editable)
		return PerspectiveQuadEditError::ReadOnly;
	if (IsGestureActive())
		return PerspectiveQuadEditError::GestureAlreadyActive;
	target.reset();
	validation.reset();
	ClearGesture();
	return PerspectiveQuadEditError::None;
}

void PerspectiveQuadEditState::Clear() {
	current_quad.reset();
	target.reset();
	validation.reset();
	bound = false;
	editable = false;
	ClearGesture();
}

bool PerspectiveQuadEditState::IsModified() const noexcept {
	if (!target)
		return false;
	return !current_quad || !SameQuad(*current_quad, *target);
}

PerspectiveQuadEditError PerspectiveQuadEditState::BeginGesture(
	Vec2 point,
	double tolerance) {
	if (!std::isfinite(tolerance) || tolerance < 0.0)
		return PerspectiveQuadEditError::InvalidTolerance;
	if (auto const pointer_error = ValidatePointer(point);
		pointer_error != PerspectiveQuadEditError::None)
		return pointer_error;
	if (!bound)
		return PerspectiveQuadEditError::NotBound;
	if (!target)
		return PerspectiveQuadEditError::NoTarget;
	if (!editable)
		return PerspectiveQuadEditError::ReadOnly;
	if (IsGestureActive())
		return PerspectiveQuadEditError::GestureAlreadyActive;

	return BeginGesture(HitTestPerspectiveQuad(*target, point, tolerance), point);
}

PerspectiveQuadEditError PerspectiveQuadEditState::BeginGesture(
	PerspectiveQuadHandle handle,
	Vec2 point) {
	if (!bound)
		return PerspectiveQuadEditError::NotBound;
	if (!target)
		return PerspectiveQuadEditError::NoTarget;
	if (!editable)
		return PerspectiveQuadEditError::ReadOnly;
	if (IsGestureActive())
		return PerspectiveQuadEditError::GestureAlreadyActive;
	if (!IsEditableHandle(handle))
		return PerspectiveQuadEditError::InvalidHandle;
	if (auto const pointer_error = ValidatePointer(point);
		pointer_error != PerspectiveQuadEditError::None)
		return pointer_error;

	gesture_start_target = target;
	gesture_start_validation = validation;
	gesture_start_pointer = point;
	active_handle = handle;
	return PerspectiveQuadEditError::None;
}

PerspectiveQuadEditError PerspectiveQuadEditState::UpdateGesture(
	Vec2 point,
	PerspectiveQuadGestureModifiers modifiers) {
	if (!gesture_start_target || !target)
		return PerspectiveQuadEditError::NoGesture;
	if (auto const pointer_error = ValidatePointer(point);
		pointer_error != PerspectiveQuadEditError::None)
		return pointer_error;

	Vec2 delta = point - gesture_start_pointer;
	if (modifiers.axis_locked) {
		if (std::abs(delta.x) >= std::abs(delta.y))
			delta.y = 0.0;
		else
			delta.x = 0.0;
	}

	Quad candidate = *gesture_start_target;
	auto const move_corner = [&candidate](std::size_t index, Vec2 corner_delta) {
		candidate[index] = candidate[index] + corner_delta;
	};
	Vec2 const mirrored_delta {-delta.x, -delta.y};
	if (auto const corner = CornerIndex(active_handle)) {
		move_corner(*corner, delta);
		if (modifiers.symmetric)
			move_corner((*corner + 2) % candidate.size(), mirrored_delta);
	}
	else if (auto const edge = EdgeCornerIndices(active_handle)) {
		move_corner((*edge)[0], delta);
		move_corner((*edge)[1], delta);
		if (modifiers.symmetric) {
			auto const opposite = OppositeEdgeCornerIndices(active_handle);
			move_corner(opposite[0], mirrored_delta);
			move_corner(opposite[1], mirrored_delta);
		}
	}
	else if (active_handle == PerspectiveQuadHandle::Center) {
		for (auto& candidate_point : candidate)
			candidate_point = candidate_point + delta;
	}
	else {
		return PerspectiveQuadEditError::InvalidHandle;
	}

	for (auto const corner : candidate) {
		if (!IsFinite(corner))
			return PerspectiveQuadEditError::NonFinitePointer;
		if (!IsInSafeRange(corner))
			return PerspectiveQuadEditError::CoordinateOutOfRange;
	}

	// Finite targets inside the numeric safety range are deliberately retained
	// even when their winding or topology is invalid, so another drag can fix
	// them without silently reordering semantic corners.
	target = candidate;
	validation = ValidateQuad(candidate);
	return PerspectiveQuadEditError::None;
}

PerspectiveQuadEditError PerspectiveQuadEditState::FinishGesture() {
	if (!IsGestureActive())
		return PerspectiveQuadEditError::NoGesture;
	ClearGesture();
	return PerspectiveQuadEditError::None;
}

PerspectiveQuadEditError PerspectiveQuadEditState::CancelGesture() {
	if (!gesture_start_target)
		return PerspectiveQuadEditError::NoGesture;
	target = *gesture_start_target;
	validation = gesture_start_validation;
	ClearGesture();
	return PerspectiveQuadEditError::None;
}

PerspectiveQuadEditError PerspectiveQuadEditState::UseCurrent() {
	if (!bound)
		return PerspectiveQuadEditError::NotBound;
	if (!editable)
		return PerspectiveQuadEditError::ReadOnly;
	if (!current_quad)
		return PerspectiveQuadEditError::NoCurrentQuad;
	target = current_quad;
	validation = ValidateQuad(*target);
	ClearGesture();
	return PerspectiveQuadEditError::None;
}

void PerspectiveQuadEditState::ClearGesture() {
	gesture_start_target.reset();
	gesture_start_validation.reset();
	gesture_start_pointer = {};
	active_handle = PerspectiveQuadHandle::None;
}

}
