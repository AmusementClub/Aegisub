#include <main.h>

#include "../../src/perspective_quad_edit_state.h"

#include <algorithm>
#include <array>
#include <limits>

namespace {
using namespace perspective;

Quad TestQuad() {
	return {{{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}}};
}

void ExpectVec(Vec2 expected, Vec2 actual) {
	EXPECT_DOUBLE_EQ(expected.x, actual.x);
	EXPECT_DOUBLE_EQ(expected.y, actual.y);
}

void ExpectQuad(Quad const& expected, Quad const& actual) {
	for (std::size_t index = 0; index < expected.size(); ++index)
		ExpectVec(expected[index], actual[index]);
}
}

TEST(perspective_quad_edit_state, bind_keeps_current_optional_and_target_explicit) {
	PerspectiveQuadEditState state;
	auto invalid = TestQuad();
	std::swap(invalid[1], invalid[3]);
	EXPECT_EQ(PerspectiveQuadEditError::InvalidSource, state.Bind(invalid, true));
	EXPECT_FALSE(state.IsBound());
	EXPECT_FALSE(state.Current());
	EXPECT_FALSE(state.Target());
	EXPECT_FALSE(state.Validation());

	EXPECT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), true));
	EXPECT_TRUE(state.IsBound());
	EXPECT_TRUE(state.IsEditable());
	EXPECT_TRUE(state.CanInitializeFromCurrent());
	EXPECT_FALSE(state.HasTarget());
	EXPECT_FALSE(state.IsModified());
	EXPECT_FALSE(state.Validation());
	ASSERT_EQ(PerspectiveQuadEditError::None, state.UseCurrent());
	EXPECT_TRUE(state.HasTarget());
	ASSERT_TRUE(state.Validation());
	EXPECT_EQ(GeometryError::None, state.Validation()->error);
	EXPECT_FALSE(state.IsModified());
	EXPECT_FALSE(state.CanInitializeFromCurrent());
	state.Clear();
	EXPECT_FALSE(state.IsBound());
	EXPECT_FALSE(state.IsEditable());
}

TEST(perspective_quad_edit_state, target_can_be_drawn_without_a_current_quad) {
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(std::nullopt, true));
	EXPECT_TRUE(state.IsBound());
	EXPECT_TRUE(state.IsEditable());
	EXPECT_FALSE(state.CanInitializeFromCurrent());
	EXPECT_EQ(PerspectiveQuadEditError::NoCurrentQuad, state.UseCurrent());
	EXPECT_EQ(PerspectiveQuadEditError::NoTarget,
		state.BeginGesture({}, 1.0));

	ASSERT_EQ(PerspectiveQuadEditError::None, state.ReplaceTarget(TestQuad()));
	EXPECT_TRUE(state.HasTarget());
	EXPECT_TRUE(state.IsModified());
	ASSERT_TRUE(state.Validation());
	EXPECT_EQ(GeometryError::None, state.Validation()->error);
}

TEST(perspective_quad_edit_state, refresh_updates_current_without_losing_editable_target) {
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), true, true));
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::TopRight, {10.0, 0.0}));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.UpdateGesture({13.0, 2.0}));
	EXPECT_EQ(PerspectiveQuadEditError::GestureAlreadyActive,
		state.RefreshCurrent(TestQuad(), true));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.FinishGesture());
	auto const target = *state.Target();

	auto refreshed_current = TestQuad();
	for (auto& point : refreshed_current) {
		point.x += 20.0;
		point.y -= 5.0;
	}
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.RefreshCurrent(refreshed_current, true));
	EXPECT_FALSE(state.IsGestureActive());
	ASSERT_TRUE(state.Current());
	ExpectQuad(refreshed_current, *state.Current());
	ASSERT_TRUE(state.Target());
	ExpectQuad(target, *state.Target());
	EXPECT_TRUE(state.IsModified());

	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.RefreshCurrent(refreshed_current, false));
	EXPECT_FALSE(state.IsEditable());
	ASSERT_TRUE(state.Target());
	ExpectQuad(refreshed_current, *state.Target());
	EXPECT_FALSE(state.IsModified());
}

TEST(perspective_quad_edit_state, hit_test_keeps_semantic_corner_order_before_center) {
	auto const quad = TestQuad();
	EXPECT_EQ(PerspectiveQuadHandle::TopLeft,
		HitTestPerspectiveQuad(quad, {0.0, 0.0}, 0.0));
	EXPECT_EQ(PerspectiveQuadHandle::TopRight,
		HitTestPerspectiveQuad(quad, {10.0, 0.0}, 0.0));
	EXPECT_EQ(PerspectiveQuadHandle::BottomRight,
		HitTestPerspectiveQuad(quad, {10.0, 10.0}, 0.0));
	EXPECT_EQ(PerspectiveQuadHandle::BottomLeft,
		HitTestPerspectiveQuad(quad, {0.0, 10.0}, 0.0));
	EXPECT_EQ(PerspectiveQuadHandle::Center,
		HitTestPerspectiveQuad(quad, {5.0, 5.0}, 1.0));

	// With an intentionally broad tolerance all five handles overlap. Fixed
	// semantic corner order wins rather than screen-relative reordering.
	EXPECT_EQ(PerspectiveQuadHandle::TopLeft,
		HitTestPerspectiveQuad(quad, {5.0, 5.0}, 8.0));
	EXPECT_EQ(PerspectiveQuadHandle::None,
		HitTestPerspectiveQuad(quad, {30.0, 30.0}, 1.0));
	EXPECT_EQ(PerspectiveQuadHandle::None,
		HitTestPerspectiveQuad(quad, {5.0, 5.0}, -1.0));
}

TEST(perspective_quad_edit_state, opposite_corner_drags_always_create_semantic_order) {
	Quad const expected {{{2.0, 3.0}, {9.0, 3.0}, {9.0, 8.0}, {2.0, 8.0}}};
	ExpectQuad(expected, PerspectiveQuadFromOppositeCorners({2.0, 3.0}, {9.0, 8.0}));
	ExpectQuad(expected, PerspectiveQuadFromOppositeCorners({9.0, 3.0}, {2.0, 8.0}));
	ExpectQuad(expected, PerspectiveQuadFromOppositeCorners({9.0, 8.0}, {2.0, 3.0}));
	ExpectQuad(expected, PerspectiveQuadFromOppositeCorners({2.0, 8.0}, {9.0, 3.0}));
}

TEST(perspective_quad_edit_state, opposite_corner_drag_follows_current_edge_direction) {
	struct Case {
		Vec2 direction;
		Quad expected;
	};
	std::array<Case, 4> const cases {{
		{{1.0, 0.0}, {{{2.0, 3.0}, {9.0, 3.0}, {9.0, 8.0}, {2.0, 8.0}}}},
		{{0.0, 1.0}, {{{9.0, 3.0}, {9.0, 8.0}, {2.0, 8.0}, {2.0, 3.0}}}},
		{{-1.0, 0.0}, {{{9.0, 8.0}, {2.0, 8.0}, {2.0, 3.0}, {9.0, 3.0}}}},
		{{0.0, -1.0}, {{{2.0, 8.0}, {2.0, 3.0}, {9.0, 3.0}, {9.0, 8.0}}}},
	}};
	for (auto const& value : cases) {
		auto const forward_drag = PerspectiveQuadFromOppositeCorners(
			{2.0, 3.0}, {9.0, 8.0}, value.direction);
		auto const reverse_drag = PerspectiveQuadFromOppositeCorners(
			{9.0, 8.0}, {2.0, 3.0}, value.direction);
		ExpectQuad(value.expected, forward_drag);
		ExpectQuad(value.expected, reverse_drag);
		EXPECT_TRUE(ValidateQuad(forward_drag));
		EXPECT_TRUE(ValidateQuad(reverse_drag));
	}
}

TEST(perspective_quad_edit_state, corner_orientation_has_deterministic_fallbacks) {
	Quad const axis_aligned {{{2.0, 3.0}, {9.0, 3.0}, {9.0, 8.0}, {2.0, 8.0}}};
	Quad const left_first {{{9.0, 8.0}, {2.0, 8.0}, {2.0, 3.0}, {9.0, 3.0}}};
	ExpectQuad(axis_aligned,
		PerspectiveQuadFromOppositeCorners({2.0, 3.0}, {9.0, 8.0}, std::nullopt));
	ExpectQuad(axis_aligned,
		PerspectiveQuadFromOppositeCorners({2.0, 3.0}, {9.0, 8.0}, Vec2 {}));
	ExpectQuad(axis_aligned,
		PerspectiveQuadFromOppositeCorners(
			{2.0, 3.0}, {9.0, 8.0},
			Vec2 {std::numeric_limits<double>::infinity(), 0.0}));
	// Exact diagonal ties select the horizontal axis.
	ExpectQuad(axis_aligned,
		PerspectiveQuadFromOppositeCorners(
			{2.0, 3.0}, {9.0, 8.0}, Vec2 {1.0, 1.0}));
	ExpectQuad(left_first,
		PerspectiveQuadFromOppositeCorners(
			{2.0, 3.0}, {9.0, 8.0}, Vec2 {-1.0, 1.0}));
}

TEST(perspective_quad_edit_state, creation_requires_movement_past_tolerance_on_either_axis) {
	EXPECT_TRUE(IsPerspectiveQuadCreationDrag({10.0, 10.0}, {16.0, 4.0}, 5.0));
	// Single-axis drags are intentional quads, not mis-clicks.
	EXPECT_TRUE(IsPerspectiveQuadCreationDrag({10.0, 10.0}, {15.0, 20.0}, 5.0));
	EXPECT_TRUE(IsPerspectiveQuadCreationDrag({10.0, 10.0}, {20.0, 15.0}, 5.0));
	EXPECT_TRUE(IsPerspectiveQuadCreationDrag({10.0, 10.0}, {30.0, 10.0}, 5.0));
	EXPECT_FALSE(IsPerspectiveQuadCreationDrag({10.0, 10.0}, {15.0, 14.0}, 5.0));
	EXPECT_FALSE(IsPerspectiveQuadCreationDrag(
		{10.0, 10.0}, {std::numeric_limits<double>::infinity(), 20.0}, 5.0));
	EXPECT_FALSE(IsPerspectiveQuadCreationDrag(
		{10.0, 10.0}, {20.0, 20.0},
		std::numeric_limits<double>::infinity()));
	EXPECT_FALSE(IsPerspectiveQuadCreationDrag(
		{10.0, 10.0}, {20.0, 20.0}, -1.0));
}

TEST(perspective_quad_edit_state, arithmetic_center_survives_finite_invalid_target) {
	Quad const bow_tie {{{0.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {10.0, 0.0}}};
	EXPECT_EQ(GeometryError::SelfIntersecting, ValidateQuad(bow_tie).error);
	auto const center = PerspectiveQuadArithmeticCenter(bow_tie);
	ASSERT_TRUE(center);
	ExpectVec({5.0, 5.0}, *center);
	// The bow-tie's TL-TR edge midpoint coincides with the arithmetic center;
	// the more specific edge handle wins there, but the quad stays grabbable.
	EXPECT_EQ(PerspectiveQuadHandle::EdgeTop,
		HitTestPerspectiveQuad(bow_tie, *center, 0.0));

	auto non_finite = bow_tie;
	non_finite[2].x = std::numeric_limits<double>::infinity();
	EXPECT_FALSE(PerspectiveQuadArithmeticCenter(non_finite));
	EXPECT_EQ(PerspectiveQuadHandle::None,
		HitTestPerspectiveQuad(non_finite, {5.0, 5.0}, 1.0));
}

TEST(perspective_quad_edit_state, corner_drag_uses_gesture_start_and_finish_keeps_target) {
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), true, true));
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::TopRight, {11.0, 1.0}));
	EXPECT_TRUE(state.IsGestureActive());
	EXPECT_EQ(PerspectiveQuadHandle::TopRight, state.ActiveHandle());

	ASSERT_EQ(PerspectiveQuadEditError::None, state.UpdateGesture({12.0, 3.0}));
	ASSERT_TRUE(state.Target());
	ExpectVec({11.0, 2.0}, (*state.Target())[1]);
	// The second update is still relative to the original target and pointer,
	// rather than accumulating the previous mouse-move delta.
	ASSERT_EQ(PerspectiveQuadEditError::None, state.UpdateGesture({13.0, 2.0}));
	ExpectVec({12.0, 1.0}, (*state.Target())[1]);
	ExpectVec({0.0, 0.0}, (*state.Target())[0]);
	ExpectVec({10.0, 10.0}, (*state.Target())[2]);
	ASSERT_TRUE(state.Validation());
	EXPECT_EQ(GeometryError::None, state.Validation()->error);

	EXPECT_EQ(PerspectiveQuadEditError::None, state.FinishGesture());
	EXPECT_FALSE(state.IsGestureActive());
	EXPECT_TRUE(state.IsModified());
	ExpectVec({12.0, 1.0}, (*state.Target())[1]);
}

TEST(perspective_quad_edit_state, cancel_restores_gesture_start_and_use_current_restores_current) {
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), true, true));
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::BottomRight, {10.0, 10.0}));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.UpdateGesture({12.0, 12.0}));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.FinishGesture());
	auto const previously_finished = *state.Target();

	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::Center, {5.5, 5.5}));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.UpdateGesture({2.5, 9.5}));
	EXPECT_NE(previously_finished[0].x, (*state.Target())[0].x);
	EXPECT_EQ(PerspectiveQuadEditError::None, state.CancelGesture());
	ExpectQuad(previously_finished, *state.Target());
	EXPECT_TRUE(state.IsModified());

	EXPECT_EQ(PerspectiveQuadEditError::None, state.UseCurrent());
	ExpectQuad(*state.Current(), *state.Target());
	EXPECT_FALSE(state.IsModified());
}

TEST(perspective_quad_edit_state, finite_invalid_target_is_retained_and_can_be_repaired) {
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), true, true));
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::TopRight, {10.0, 0.0}));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.UpdateGesture({-2.0, 12.0}));
	ASSERT_TRUE(state.Validation());
	EXPECT_NE(GeometryError::None, state.Validation()->error);
	ExpectVec({-2.0, 12.0}, (*state.Target())[1]);
	ASSERT_EQ(PerspectiveQuadEditError::None, state.FinishGesture());

	// Invalid topology does not make the state read-only or erase handles.
	EXPECT_TRUE(state.IsEditable());
	EXPECT_EQ(PerspectiveQuadHandle::TopRight,
		HitTestPerspectiveQuad(*state.Target(), {-2.0, 12.0}, 0.0));
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::TopRight, {-2.0, 12.0}));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.UpdateGesture({8.0, 0.0}));
	ASSERT_TRUE(state.Validation());
	EXPECT_EQ(GeometryError::None, state.Validation()->error);
	ExpectVec({8.0, 0.0}, (*state.Target())[1]);
}

TEST(perspective_quad_edit_state, duplicate_and_reversed_targets_keep_fixed_corner_indices) {
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), true, true));

	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::TopRight, {10.0, 0.0}));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.UpdateGesture({0.0, 0.0}));
	ASSERT_TRUE(state.Validation());
	EXPECT_EQ(GeometryError::DuplicatePoint, state.Validation()->error);
	ExpectVec({0.0, 0.0}, (*state.Target())[0]);
	ExpectVec({0.0, 0.0}, (*state.Target())[1]);
	ASSERT_EQ(PerspectiveQuadEditError::None, state.FinishGesture());

	// Continue editing the same semantic TR index, then move BL. The resulting
	// order is counter-clockwise and must be rejected without swapping corners.
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::TopRight, {0.0, 0.0}));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.UpdateGesture({0.0, 10.0}));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.FinishGesture());
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::BottomLeft, {0.0, 10.0}));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.UpdateGesture({10.0, 0.0}));
	ASSERT_TRUE(state.Validation());
	EXPECT_EQ(GeometryError::WrongWinding, state.Validation()->error);
	ExpectQuad({{{0.0, 0.0}, {0.0, 10.0}, {10.0, 10.0}, {10.0, 0.0}}},
		*state.Target());
}

TEST(perspective_quad_edit_state, center_drag_translates_all_ordered_corners_equally) {
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), true, true));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.BeginGesture({5.0, 5.0}, 0.5));
	EXPECT_EQ(PerspectiveQuadHandle::Center, state.ActiveHandle());
	ASSERT_EQ(PerspectiveQuadEditError::None, state.UpdateGesture({-25.0, 10.0}));
	ASSERT_TRUE(state.Target());
	ExpectVec({-30.0, 5.0}, (*state.Target())[0]);
	ExpectVec({-20.0, 5.0}, (*state.Target())[1]);
	ExpectVec({-20.0, 15.0}, (*state.Target())[2]);
	ExpectVec({-30.0, 15.0}, (*state.Target())[3]);
	EXPECT_EQ(GeometryError::None, state.Validation()->error);
}

TEST(perspective_quad_edit_state, hit_test_finds_edge_midpoints_after_corners) {
	auto const quad = TestQuad();
	EXPECT_EQ(PerspectiveQuadHandle::EdgeTop,
		HitTestPerspectiveQuad(quad, {5.0, 0.0}, 0.0));
	EXPECT_EQ(PerspectiveQuadHandle::EdgeRight,
		HitTestPerspectiveQuad(quad, {10.0, 5.0}, 0.0));
	EXPECT_EQ(PerspectiveQuadHandle::EdgeBottom,
		HitTestPerspectiveQuad(quad, {5.0, 10.0}, 0.0));
	EXPECT_EQ(PerspectiveQuadHandle::EdgeLeft,
		HitTestPerspectiveQuad(quad, {0.0, 5.0}, 0.0));
	// Corners keep priority over edges when a broad tolerance overlaps both.
	EXPECT_EQ(PerspectiveQuadHandle::TopLeft,
		HitTestPerspectiveQuad(quad, {1.0, 1.0}, 3.0));
}

TEST(perspective_quad_edit_state, edge_drag_moves_both_corners_of_the_edge) {
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), true, true));
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::EdgeTop, {5.0, 0.0}));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.UpdateGesture({5.0, -2.0}));
	ExpectVec({0.0, -2.0}, (*state.Target())[0]);
	ExpectVec({10.0, -2.0}, (*state.Target())[1]);
	ExpectVec({10.0, 10.0}, (*state.Target())[2]);
	ExpectVec({0.0, 10.0}, (*state.Target())[3]);
}

TEST(perspective_quad_edit_state, modifiers_constrain_and_mirror_corner_drags) {
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), true, true));
	PerspectiveQuadGestureModifiers const axis_locked {true, false};
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::TopRight, {10.0, 0.0}));
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.UpdateGesture({14.0, -3.0}, axis_locked));
	// |dx| dominates, so dy is clamped away.
	ExpectVec({14.0, 0.0}, (*state.Target())[1]);
	ASSERT_EQ(PerspectiveQuadEditError::None, state.FinishGesture());

	PerspectiveQuadGestureModifiers const symmetric {false, true};
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::TopRight, {14.0, 0.0}));
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.UpdateGesture({17.0, 2.0}, symmetric));
	ExpectVec({17.0, 2.0}, (*state.Target())[1]);
	// The opposite corner (BottomLeft) mirrors the delta through the center.
	ExpectVec({-3.0, 8.0}, (*state.Target())[3]);
}

TEST(perspective_quad_edit_state, symmetric_edge_drag_moves_the_opposite_edge_too) {
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), true, true));
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::EdgeTop, {5.0, 0.0}));
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.UpdateGesture({5.0, 3.0}, {false, true}));
	ExpectVec({0.0, 3.0}, (*state.Target())[0]);
	ExpectVec({10.0, 3.0}, (*state.Target())[1]);
	ExpectVec({10.0, 7.0}, (*state.Target())[2]);
	ExpectVec({0.0, 7.0}, (*state.Target())[3]);
}

TEST(perspective_quad_edit_state, drop_target_keeps_binding_and_current_quad) {
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), true, true));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.UseCurrent());
	ASSERT_EQ(PerspectiveQuadEditError::None, state.DropTarget());
	EXPECT_TRUE(state.IsBound());
	EXPECT_TRUE(state.IsEditable());
	ASSERT_TRUE(state.Current());
	EXPECT_FALSE(state.HasTarget());
	EXPECT_FALSE(state.Validation());
	EXPECT_FALSE(state.IsModified());
	EXPECT_EQ(PerspectiveQuadEditError::NoTarget,
		state.BeginGesture(PerspectiveQuadHandle::TopLeft, {0.0, 0.0}));
}

TEST(perspective_quad_edit_state, drop_target_rejects_unbound_read_only_and_gesture) {
	PerspectiveQuadEditState state;
	EXPECT_EQ(PerspectiveQuadEditError::NotBound, state.DropTarget());
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), false, true));
	EXPECT_EQ(PerspectiveQuadEditError::ReadOnly, state.DropTarget());
	state.Clear();
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), true, true));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.UseCurrent());
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::Center, {5.0, 5.0}));
	EXPECT_EQ(PerspectiveQuadEditError::GestureAlreadyActive, state.DropTarget());
}

TEST(perspective_quad_edit_state, read_only_binding_rejects_all_edit_entry_points) {
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), false, true));
	EXPECT_TRUE(state.IsBound());
	EXPECT_FALSE(state.IsEditable());
	EXPECT_EQ(PerspectiveQuadEditError::ReadOnly,
		state.BeginGesture({0.0, 0.0}, 1.0));
	EXPECT_EQ(PerspectiveQuadEditError::ReadOnly,
		state.BeginGesture(PerspectiveQuadHandle::TopLeft, {0.0, 0.0}));
	EXPECT_EQ(PerspectiveQuadEditError::ReadOnly, state.UseCurrent());
	ExpectQuad(*state.Current(), *state.Target());
	EXPECT_FALSE(state.IsModified());
}

TEST(perspective_quad_edit_state, unsafe_pointer_updates_are_rejected_without_losing_gesture) {
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), true, true));
	EXPECT_EQ(PerspectiveQuadEditError::NonFinitePointer,
		state.BeginGesture(PerspectiveQuadHandle::TopLeft,
			{std::numeric_limits<double>::quiet_NaN(), 0.0}));
	EXPECT_EQ(PerspectiveQuadEditError::CoordinateOutOfRange,
		state.BeginGesture(PerspectiveQuadHandle::TopLeft,
			{MaxAbsCoordinate + 1.0, 0.0}));

	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::Center, {5.0, 5.0}));
	auto const before = *state.Target();
	EXPECT_EQ(PerspectiveQuadEditError::NonFinitePointer,
		state.UpdateGesture({std::numeric_limits<double>::infinity(), 5.0}));
	ExpectQuad(before, *state.Target());
	EXPECT_TRUE(state.IsGestureActive());
	EXPECT_EQ(PerspectiveQuadEditError::CoordinateOutOfRange,
		state.UpdateGesture({MaxAbsCoordinate, 5.0}));
	ExpectQuad(before, *state.Target());
	EXPECT_TRUE(state.IsGestureActive());

	EXPECT_EQ(PerspectiveQuadEditError::None, state.UpdateGesture({7.0, 8.0}));
	ExpectVec({2.0, 3.0}, (*state.Target())[0]);
}

TEST(perspective_quad_edit_state, invalid_gesture_requests_leave_state_unchanged) {
	PerspectiveQuadEditState state;
	EXPECT_EQ(PerspectiveQuadEditError::NotBound,
		state.BeginGesture(PerspectiveQuadHandle::TopLeft, {}));
	EXPECT_EQ(PerspectiveQuadEditError::NoGesture, state.UpdateGesture({}));
	EXPECT_EQ(PerspectiveQuadEditError::NoGesture, state.FinishGesture());
	EXPECT_EQ(PerspectiveQuadEditError::NoGesture, state.CancelGesture());
	EXPECT_EQ(PerspectiveQuadEditError::NotBound, state.UseCurrent());

	ASSERT_EQ(PerspectiveQuadEditError::None, state.Bind(TestQuad(), true, true));
	EXPECT_EQ(PerspectiveQuadEditError::InvalidTolerance,
		state.BeginGesture({0.0, 0.0},
			std::numeric_limits<double>::infinity()));
	EXPECT_EQ(PerspectiveQuadEditError::InvalidHandle,
		state.BeginGesture({30.0, 30.0}, 1.0));
	EXPECT_EQ(PerspectiveQuadEditError::InvalidHandle,
		state.BeginGesture(PerspectiveQuadHandle::None, {}));
	EXPECT_EQ(PerspectiveQuadEditError::InvalidHandle,
		state.BeginGesture(static_cast<PerspectiveQuadHandle>(999), {}));
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.BeginGesture(PerspectiveQuadHandle::TopLeft, {}));
	EXPECT_EQ(PerspectiveQuadEditError::GestureAlreadyActive,
		state.BeginGesture(PerspectiveQuadHandle::BottomRight, {10.0, 10.0}));
	EXPECT_STREQ("a quad edit gesture is already active",
		DescribePerspectiveQuadEditError(
			PerspectiveQuadEditError::GestureAlreadyActive));
}
