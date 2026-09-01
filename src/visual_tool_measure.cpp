#include "visual_tool_measure.h"

#include "async_video_provider.h"
#include "auto4_base.h"
#include "ass_file.h"
#include "compat.h"
#include "format.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "project.h"
#include "selection_controller.h"
#include "utils.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_overlay_draw_context.h"
#include "video_overlay_draw_context_legacy_gl.h"
#include "visual_guide_interaction.h"
#include "visual_guide_overlay.h"

#include <libaegisub/color.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <wx/event.h>
#include <wx/toolbar.h>
#include <wx/translation.h>

namespace {
constexpr float kHandleRadius = 3.0f;
constexpr float kHitTolerance = 5.0f;
constexpr auto kPerspectiveFitTextOption = "Tool/Visual/Perspective/Fit Text";
constexpr auto kPerspectiveFaxFrzOnlyOption = "Tool/Visual/Perspective/Fax Frz Only";
constexpr auto kPerspectiveDecimalPlacesOption = "Tool/Visual/Perspective/Decimal Places";

VisualGuidePoint ToGuidePoint(perspective::Vec2 point) {
	return {point.x, point.y};
}

perspective::Vec2 ToPerspectivePoint(VisualGuidePoint point) {
	return {point.x, point.y};
}

VisualGuide const* FindGuide(
	VisualGuideSnapshotView const& snapshot,
	std::string const& id) {
	auto const found = std::find_if(
		snapshot.guides.begin(), snapshot.guides.end(), [&id](VisualGuide const& guide) {
			return guide.id == id;
		});
	return found == snapshot.guides.end() ? nullptr : &*found;
}

VisualGuideOverlayStyle MakeOverlayStyle(
	agi::OptionValue const* line_colour,
	agi::OptionValue const* highlight_colour) {
	VisualGuideOverlayStyle style;
	style.line_colour = to_wx(line_colour->GetColor());
	style.highlight_colour = to_wx(highlight_colour->GetColor());
	// Fixed white label border for contrast; the selected state borrows the
	// highlight colour instead. Not user-configurable to avoid a new option.
	style.label_border_colour = wxColour(255, 255, 255);
	style.label_font_size = OPT_GET("Tool/Visual/Coordinate Font Size")->GetInt();
	return style;
}

wxString MaximumGuidesStatusMessage() {
	return fmt_tl(
		"A maximum of %d visual guides is allowed.",
		static_cast<int>(VisualGuideController::MaximumGuideCount));
}

std::string DescribePerspectiveFailure(
	perspective::PerspectivePlanDiagnostic const& diagnostic) {
	using perspective::PerspectivePlanError;
	switch (diagnostic.error) {
		case PerspectivePlanError::InvalidTarget:
			return perspective::DescribeGeometryError(diagnostic.geometry_error);
		case PerspectivePlanError::StateEvaluationFailed:
		case PerspectivePlanError::StagedStateEvaluationFailed:
			return perspective::DescribeAssStateError(diagnostic.state_error);
		case PerspectivePlanError::ApplyBlocked:
		case PerspectivePlanError::StagedApplyBlocked:
			return perspective::DescribeAssApplyBlocker(diagnostic.apply_blocker);
		case PerspectivePlanError::BoundsEvaluationFailed:
		case PerspectivePlanError::StagedBoundsEvaluationFailed:
			return perspective::DescribeAssBoundsError(diagnostic.bounds_error);
		case PerspectivePlanError::ForwardEvaluationFailed:
		case PerspectivePlanError::StagedForwardEvaluationFailed:
			return perspective::DescribeForwardError(diagnostic.forward_error);
		case PerspectivePlanError::SolverFailed:
			return perspective::DescribeSolverError(diagnostic.solver_error);
		case PerspectivePlanError::RewriteFailed:
			return perspective::DescribeRewriteError(diagnostic.rewrite_error);
		case PerspectivePlanError::ResidualFailed:
			return perspective::DescribeResidualError(diagnostic.residual_error);
		default:
			return perspective::DescribePerspectivePlanError(diagnostic.error);
	}
}
}

VisualToolMeasure::VisualToolMeasure(VideoDisplay *parent, agi::Context *context)
: VisualToolBase(parent, context)
, controller(context->GetUI().visualGuideController)
, text(agi::make_unique<OpenGLText>())
, fit_text_to_target(OPT_GET(kPerspectiveFitTextOption)->GetBool())
, fax_frz_only(OPT_GET(kPerspectiveFaxFrzOnlyOption)->GetBool()) {
	auto core = c->GetCore();
	connections.push_back(core.project->AddVideoProviderListener([this](AsyncVideoProvider*) {
		CancelInteraction(false);
		if (submode == SubMode::PerspectiveQuad)
			RebindPerspective();
		this->parent->Render();
	}));
	connections.push_back(core.project->AddTimecodesListener([this](agi::vfr::Framerate const&) {
		CancelInteraction(false);
		if (submode == SubMode::PerspectiveQuad)
			RebindPerspective();
		this->parent->Render();
	}));
	auto subscribe_solver_option = [this](char const* name, bool& cached) {
		connections.push_back(OPT_SUB(name, [this, &cached](agi::OptionValue const& value) {
			bool const next = value.GetBool();
			if (cached == next)
				return;
			cached = next;
			if (this->perspective_diagnostic_depends_on_solver_options) {
				this->perspective_diagnostic.clear();
				this->perspective_diagnostic_depends_on_solver_options = false;
				this->c->ShowStatus({});
			}
			this->UpdateToolbarState();
			this->parent->Render();
		}));
	};
	subscribe_solver_option(kPerspectiveFitTextOption, fit_text_to_target);
	subscribe_solver_option(kPerspectiveFaxFrzOnlyOption, fax_frz_only);
}

VisualToolMeasure::~VisualToolMeasure() {
	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
	if (toolbar)
		toolbar->Unbind(wxEVT_TOOL, &VisualToolMeasure::OnSubTool, this);
}

std::optional<VisualGuidePoint> VisualToolMeasure::CurrentGuidePoint(bool clamp) const {
	auto point = CanvasToVisualGuide(mouse_pos, parent->GetVisualGuideViewport());
	if (!point)
		return std::nullopt;
	if (clamp)
		return ClampVisualGuidePointToScript(*point, parent->GetVisualGuideViewport());
	return point;
}

bool VisualToolMeasure::IsInsideVideoViewport(Vector2D point) const {
	auto const& viewport = parent->GetVisualGuideViewport();
	return IsVisualGuideViewportMappable(viewport)
		&& point.X() >= viewport.canvas_x
		&& point.Y() >= viewport.canvas_y
		&& point.X() <= viewport.canvas_x + viewport.canvas_width
		&& point.Y() <= viewport.canvas_y + viewport.canvas_height;
}

void VisualToolMeasure::BeginInteraction(wxMouseEvent&) {
	if (!controller || !IsInsideVideoViewport(mouse_pos))
		return;

	auto const viewport = parent->GetVisualGuideViewport();
	auto const style = MakeOverlayStyle(line_color_primary_opt, highlight_color_primary_opt);
	// Hit-testing needs to measure the info-box text so selection matches the
	// rendered rectangle; reuse the same legacy context the draw pass uses.
	LegacyVideoOverlayDrawContext hit_context(gl, *text);
	if (auto hit = HitTestVisualGuides(
		mouse_pos, controller->CaptureView(), viewport, style, hit_context, kHitTolerance)) {
		std::optional<VisualGuide> guide;
		{
			auto const snapshot = controller->CaptureView();
			if (auto const* found = FindGuide(snapshot, hit.id))
				guide = *found;
		}
		if (!guide)
			return;
		controller->Select(hit.id);

		auto point = CanvasToVisualGuide(mouse_pos, viewport);
		if (!point)
			return;

		original_guide = std::move(*guide);
		editing_id = hit.id;
		drag_start_point = ClampVisualGuidePointToScript(*point, viewport);
		switch (hit.part) {
			case VisualGuideHitPart::FirstEndpoint:
				interaction = Interaction::DraggingFirstEndpoint;
				break;
			case VisualGuideHitPart::SecondEndpoint:
				interaction = Interaction::DraggingSecondEndpoint;
				break;
			case VisualGuideHitPart::Line:
			case VisualGuideHitPart::Label:
				// Dragging the info box translates the whole guide, like the body.
				interaction = Interaction::DraggingLine;
				break;
			case VisualGuideHitPart::None:
				return;
		}
	}
	else {
		auto point = CurrentGuidePoint(true);
		if (!point)
			return;

		controller->Select(std::nullopt);
		VisualGuide preview;
		preview.id = "preview";
		preview.first = *point;
		preview.second = *point;
		preview_guide = preview;
		interaction = Interaction::CreatingMeasurement;
	}

	dragging = true;
	parent->CaptureMouse();
}

void VisualToolMeasure::UpdateInteraction() {
	if (interaction == Interaction::None)
		return;

	auto const viewport = parent->GetVisualGuideViewport();
	if (interaction == Interaction::CreatingMeasurement) {
		auto point = CurrentGuidePoint(true);
		if (point && preview_guide)
			preview_guide->second = SnapVisualGuideMeasurementPoint(
				preview_guide->first, *point, shift_down);
		return;
	}

	if (!editing_id || !original_guide)
		return;

	auto point = CanvasToVisualGuide(mouse_pos, viewport);
	if (!point)
		return;
	point = ClampVisualGuidePointToScript(*point, viewport);

	VisualGuideDragAction action = VisualGuideDragAction::MoveGuide;
	switch (interaction) {
		case Interaction::DraggingLine:
			break;
		case Interaction::DraggingFirstEndpoint:
			action = VisualGuideDragAction::MoveFirstEndpoint;
			break;
		case Interaction::DraggingSecondEndpoint:
			action = VisualGuideDragAction::MoveSecondEndpoint;
			break;
		case Interaction::None:
		case Interaction::CreatingMeasurement:
			return;
	}
	auto updated = ApplyVisualGuideDrag(
		*original_guide, action, drag_start_point, *point, shift_down, viewport);
	controller->Update(*editing_id, std::move(updated));
}

void VisualToolMeasure::FinishInteraction() {
	if (interaction == Interaction::None)
		return;

	if (interaction == Interaction::CreatingMeasurement && preview_guide) {
		auto const viewport = parent->GetVisualGuideViewport();
		auto const first = VisualGuideToCanvas(preview_guide->first, viewport);
		auto const second = VisualGuideToCanvas(preview_guide->second, viewport);
		if ((first - second).SquareLen() > kHitTolerance * kHitTolerance) {
			preview_guide->id.clear();
			if (auto id = controller->Add(*preview_guide))
				controller->Select(*id);
			else
				c->ShowStatus(from_wx(MaximumGuidesStatusMessage()));
		}
	}

	interaction = Interaction::None;
	editing_id.reset();
	original_guide.reset();
	preview_guide.reset();
	dragging = false;
	if (parent->HasCapture())
		parent->ReleaseMouse();
	parent->SetFocus();
}

void VisualToolMeasure::CancelInteraction(bool render) {
	if (interaction == Interaction::None)
		return;

	auto id = editing_id;
	auto original = original_guide;
	interaction = Interaction::None;
	editing_id.reset();
	original_guide.reset();
	preview_guide.reset();
	dragging = false;

	if (id && original)
		controller->Update(*id, std::move(*original));
	if (parent->HasCapture())
		parent->ReleaseMouse();
	if (render)
		parent->Render();
}

std::optional<perspective::Vec2> VisualToolMeasure::CurrentPerspectivePoint() const {
	auto point = CanvasToVisualGuide(mouse_pos, parent->GetVisualGuideViewport());
	if (!point)
		return std::nullopt;
	return ToPerspectivePoint(*point);
}

void VisualToolMeasure::BeginPerspectiveInteraction() {
	auto const point = CurrentPerspectivePoint();
	auto const& viewport = parent->GetVisualGuideViewport();
	if (!point || !perspective_state.IsEditable()
		|| !IsVisualGuideViewportMappable(viewport)
		|| !IsInsideVideoViewport(mouse_pos))
		return;

	// A previous Apply failure describes the old target. Once the user starts
	// repairing or replacing it, let live validation own the toolbar message.
	bool const had_diagnostic = !perspective_diagnostic.empty();
	perspective_diagnostic.clear();
	perspective_diagnostic_depends_on_solver_options = false;
	if (had_diagnostic)
		c->ShowStatus({});
	UpdateToolbarState();

	if (auto const& target = perspective_state.Target()) {
		perspective::Quad canvas_target;
		for (std::size_t index = 0; index < target->size(); ++index) {
			auto const canvas_point = VisualGuideToCanvas(
				ToGuidePoint((*target)[index]), viewport);
			canvas_target[index] = {canvas_point.X(), canvas_point.Y()};
		}
		auto const handle = perspective::HitTestPerspectiveQuad(
			canvas_target, {mouse_pos.X(), mouse_pos.Y()}, kHitTolerance);
		if (handle != perspective::PerspectiveQuadHandle::None
			&& perspective_state.BeginGesture(handle, *point)
				== perspective::PerspectiveQuadEditError::None) {
			dragging = true;
			parent->CaptureMouse();
			return;
		}
	}

	perspective_creation_anchor = *point;
	perspective_creation_canvas_anchor = mouse_pos;
	perspective_creation_preview = perspective::PerspectiveQuadFromOppositeCorners(
		*point, *point,
		perspective_source
			? perspective::PerspectiveFirstEdgeDirection(*perspective_source)
			: std::optional<perspective::Vec2> {});
	dragging = true;
	parent->CaptureMouse();
}

void VisualToolMeasure::UpdatePerspectiveInteraction() {
	if (!IsPerspectiveInteractionActive())
		return;
	if (auto point = CurrentPerspectivePoint()) {
		if (perspective_state.IsGestureActive())
			(void)perspective_state.UpdateGesture(*point);
		else if (perspective_creation_anchor)
			perspective_creation_preview =
				perspective::PerspectiveQuadFromOppositeCorners(
					*perspective_creation_anchor, *point,
					perspective_source
						? perspective::PerspectiveFirstEdgeDirection(*perspective_source)
						: std::optional<perspective::Vec2> {});
	}
	UpdateToolbarState();
}

void VisualToolMeasure::FinishPerspectiveInteraction() {
	if (!IsPerspectiveInteractionActive())
		return;

	if (perspective_state.IsGestureActive()) {
		(void)perspective_state.FinishGesture();
	}
	else if (perspective_creation_canvas_anchor && perspective_creation_preview) {
		perspective::Vec2 const first_canvas {
			perspective_creation_canvas_anchor->X(),
			perspective_creation_canvas_anchor->Y()};
		perspective::Vec2 const second_canvas {mouse_pos.X(), mouse_pos.Y()};
		if (perspective::IsPerspectiveQuadCreationDrag(
				first_canvas, second_canvas, kHitTolerance)
			&& perspective::ValidateQuad(*perspective_creation_preview))
			(void)perspective_state.ReplaceTarget(*perspective_creation_preview);
	}
	perspective_creation_anchor.reset();
	perspective_creation_canvas_anchor.reset();
	perspective_creation_preview.reset();
	dragging = false;
	if (parent->HasCapture())
		parent->ReleaseMouse();
	parent->SetFocus();
	UpdateToolbarState();

	auto const& validation = perspective_state.Validation();
	if (validation && !*validation)
		c->ShowStatus(perspective::DescribeGeometryError(validation->error));
}

void VisualToolMeasure::CancelPerspectiveInteraction(bool render) {
	if (!IsPerspectiveInteractionActive())
		return;

	if (perspective_state.IsGestureActive())
		(void)perspective_state.CancelGesture();
	perspective_creation_anchor.reset();
	perspective_creation_canvas_anchor.reset();
	perspective_creation_preview.reset();
	dragging = false;
	if (parent->HasCapture())
		parent->ReleaseMouse();
	UpdateToolbarState();
	if (render)
		parent->Render();
}

bool VisualToolMeasure::IsPerspectiveInteractionActive() const noexcept {
	return perspective_state.IsGestureActive()
		|| perspective_creation_anchor.has_value();
}

std::optional<perspective::PerspectiveApplyContext>
VisualToolMeasure::CurrentPerspectiveApplyContext() const {
	auto const viewport = parent->GetVisualGuideViewport();
	if (!IsVisualGuideViewportMappable(viewport))
		return std::nullopt;

	auto core = c->GetCore();
	auto const* provider = core.project->VideoProvider();
	if (!provider || provider->GetWidth() <= 0 || provider->GetHeight() <= 0)
		return std::nullopt;

	perspective::PerspectiveApplyContext context;
	context.frame_number = frame_number;
	context.capture_time_ms = core.videoController->TimeAtFrame(frame_number);
	context.play_resolution = {viewport.script_width, viewport.script_height};
	int layout_width = 0;
	int layout_height = 0;
	core.ass->GetLayoutResolution(layout_width, layout_height);
	if (layout_width > 0 && layout_height > 0) {
		context.layout_resolution = perspective::Resolution {
			static_cast<double>(layout_width), static_cast<double>(layout_height)};
	}
	context.video_storage_resolution = perspective::Resolution {
		static_cast<double>(provider->GetWidth()),
		static_cast<double>(provider->GetHeight())};
	// The solver budget is measured in subtitle render pixels. Canvas zoom,
	// letterboxing, pan and DPI are display-only and must not enter this mapping.
	context.output_mapping = {
		provider->GetWidth() / viewport.script_width,
		provider->GetHeight() / viewport.script_height,
	};
	return context;
}

bool VisualToolMeasure::CanApplyPerspective() const {
	return submode == SubMode::PerspectiveQuad
		&& perspective_source.has_value()
		&& perspective_state.IsBound()
		&& perspective_state.IsEditable()
		&& perspective_state.HasTarget()
		&& perspective_state.IsModified()
		&& !IsPerspectiveInteractionActive()
		&& perspective_state.Validation().has_value()
		&& static_cast<bool>(*perspective_state.Validation());
}

void VisualToolMeasure::ApplyPerspective() {
	command_session.ResetCommitId();
	auto reset_commit_id = agi::make_scope_exit(
		[this] { command_session.ResetCommitId(); });
	if (!CanApplyPerspective())
		return;
	perspective_diagnostic_depends_on_solver_options = false;

	auto const current_context = CurrentPerspectiveApplyContext();
	auto const& target = perspective_state.Target();
	if (!current_context || !target || !perspective_source) {
		perspective_diagnostic = perspective::DescribePerspectivePlanError(
			perspective::PerspectivePlanError::InvalidContext);
		UpdateToolbarState();
		c->ShowStatus(perspective_diagnostic);
		return;
	}

	auto core = c->GetCore();
	auto* const resolved = core.selectionController->GetDialogueById(
		perspective_source->fingerprint.line.id);
	if (!resolved || resolved != active_line
		|| resolved != core.selectionController->GetActiveLine()) {
		perspective_diagnostic = perspective::DescribePerspectivePlanError(
			perspective::PerspectivePlanError::StaleSource);
		UpdateToolbarState();
		c->ShowStatus(perspective_diagnostic);
		return;
	}

	auto const planned = perspective::BuildPerspectiveMutationPlan(
		*core.ass, *current_context, *perspective_source, *target, 0.1,
		&Automation4::CalculateTextExtents,
		fit_text_to_target
			? perspective::PerspectiveScalePolicy::Fit
			: perspective::PerspectiveScalePolicy::Preserve,
		fax_frz_only
			? perspective::PerspectiveRepresentationPolicy::FaxFrzOnly
			: perspective::PerspectiveRepresentationPolicy::Automatic,
		perspective::ClampPerspectiveDecimalPlaces(
			static_cast<int>(OPT_GET(kPerspectiveDecimalPlacesOption)->GetInt())));
	if (!planned) {
		if (!fit_text_to_target
			&& planned.error == perspective::PerspectivePlanError::SolverFailed
			&& planned.solver_error == perspective::SolverError::NoFeasibleCandidate) {
			perspective_diagnostic = fax_frz_only
				? from_wx(_("The target cannot preserve the current text scale using only fax and frz."))
				: from_wx(_("The target cannot preserve the current text scale."));
			perspective_diagnostic_depends_on_solver_options = true;
		}
		else if (fax_frz_only
			&& planned.error == perspective::PerspectivePlanError::SolverFailed
			&& planned.solver_error == perspective::SolverError::NoFeasibleCandidate) {
			perspective_diagnostic = from_wx(
				_("The target cannot be represented using only fax and frz."));
			perspective_diagnostic_depends_on_solver_options = true;
		}
		else
			perspective_diagnostic = DescribePerspectiveFailure(planned);
		UpdateToolbarState();
		c->ShowStatus(perspective_diagnostic);
		parent->Render();
		return;
	}

	if (resolved != active_line
		|| resolved != core.selectionController->GetActiveLine()) {
		perspective_diagnostic = perspective::DescribePerspectivePlanError(
			perspective::PerspectivePlanError::StaleSource);
		UpdateToolbarState();
		c->ShowStatus(perspective_diagnostic);
		return;
	}
	auto const executed = perspective::ExecutePerspectiveMutationPlan(
		*core.ass, *current_context, *planned.plan);
	if (!executed) {
		perspective_diagnostic = DescribePerspectiveFailure(executed);
		UpdateToolbarState();
		c->ShowStatus(perspective_diagnostic);
		return;
	}

	AssDialogue const* changed[] = {executed.line};
	command_session.CommitWithFeedback(
		from_wx(_("apply Perspective quad")),
		AssFile::COMMIT_DIAG_TEXT,
		-1,
		executed.line,
		changed,
		aegisub::LocalCommitFeedback::ObserveSelf);
}

void VisualToolMeasure::ClearPerspectiveBinding() {
	CancelPerspectiveInteraction(false);
	perspective_state.Clear();
	perspective_source.reset();
	perspective_diagnostic.clear();
	perspective_diagnostic_depends_on_solver_options = false;
	UpdateToolbarState();
}

void VisualToolMeasure::RebindPerspective(
	PerspectiveRebindMode mode,
	bool announce_diagnostic) {
	CancelPerspectiveInteraction(false);
	if (submode != SubMode::PerspectiveQuad) {
		ClearPerspectiveBinding();
		return;
	}
	perspective_diagnostic.clear();
	perspective_diagnostic_depends_on_solver_options = false;

	auto set_diagnostic = [&](std::string message) {
		ClearPerspectiveBinding();
		perspective_diagnostic = std::move(message);
		UpdateToolbarState();
		if (announce_diagnostic)
			c->ShowStatus(perspective_diagnostic);
	};

	auto const current_context = CurrentPerspectiveApplyContext();
	if (!current_context) {
		set_diagnostic(from_wx(_("Perspective Quad requires a valid video coordinate system.")));
		return;
	}
	if (!active_line) {
		set_diagnostic(from_wx(_("Perspective Quad requires an active subtitle line on this frame.")));
		return;
	}

	auto core = c->GetCore();
	auto captured = perspective::CapturePerspectiveSource(
		*core.ass, *active_line, *current_context,
		&Automation4::CalculateTextExtents);
	if (!captured) {
		set_diagnostic(DescribePerspectiveFailure(captured));
		return;
	}

	bool const editable =
		captured.source->apply_blocker == perspective::AssApplyBlocker::None;
	perspective::PerspectiveQuadEditError bind_error =
		perspective::PerspectiveQuadEditError::InvalidSource;
	switch (mode) {
		case PerspectiveRebindMode::PreserveTarget:
			bind_error = perspective_state.RefreshCurrent(
				captured.source->current_quad, editable);
			break;
		case PerspectiveRebindMode::AcceptCurrent:
			bind_error = perspective_state.Bind(
				captured.source->current_quad,
				editable,
				captured.source->current_quad.has_value());
			break;
		case PerspectiveRebindMode::DiscardTarget:
			bind_error = perspective_state.Bind(
				captured.source->current_quad,
				editable,
				!editable && captured.source->current_quad.has_value());
			break;
	}
	if (bind_error != perspective::PerspectiveQuadEditError::None) {
		set_diagnostic(from_wx(_("Perspective Quad could not bind the evaluated geometry.")));
		return;
	}
	perspective_source = std::move(*captured.source);
	if (perspective_source->apply_blocker != perspective::AssApplyBlocker::None) {
		perspective_diagnostic = perspective::DescribeAssApplyBlocker(
			perspective_source->apply_blocker);
	}
	UpdateToolbarState();
	if (announce_diagnostic && !perspective_diagnostic.empty())
		c->ShowStatus(perspective_diagnostic);
}

void VisualToolMeasure::OnMouseEvent(wxMouseEvent& event) {
	shift_down = event.ShiftDown();
	ctrl_down = event.CmdDown();
	alt_down = event.AltDown();
	if (event.Leaving() && interaction == Interaction::None
		&& !IsPerspectiveInteractionActive()) {
		mouse_pos = Vector2D();
		parent->RenderToolFeedback();
		return;
	}

	auto const point = event.GetPosition();
	mouse_pos = Vector2D(point.x, point.y);
	if (event.ButtonDown())
		parent->SetFocus();

	if (submode == SubMode::Segment) {
		if (event.LeftDown())
			BeginInteraction(event);
		else if (interaction != Interaction::None) {
			if (event.LeftIsDown())
				UpdateInteraction();
			else {
				// wx can deliver the release position without a preceding motion event.
				// Apply it before committing so guides end exactly where the user let go.
				UpdateInteraction();
				FinishInteraction();
			}
		}
	}
	else {
		if (event.LeftDown())
			BeginPerspectiveInteraction();
		else if (IsPerspectiveInteractionActive()) {
			if (event.LeftIsDown())
				UpdatePerspectiveInteraction();
			else {
				UpdatePerspectiveInteraction();
				FinishPerspectiveInteraction();
			}
		}
	}

	parent->RenderToolFeedback();
}

bool VisualToolMeasure::OnKeyDown(wxKeyEvent& event) {
	if (event.GetKeyCode() == WXK_ESCAPE) {
		if (submode == SubMode::Segment && interaction != Interaction::None) {
			CancelInteraction();
			return true;
		}
		if (submode == SubMode::PerspectiveQuad
			&& IsPerspectiveInteractionActive()) {
			CancelPerspectiveInteraction();
			return true;
		}
	}
	if (submode == SubMode::PerspectiveQuad
		&& (event.GetKeyCode() == WXK_RETURN
			|| event.GetKeyCode() == WXK_NUMPAD_ENTER)
		&& CanApplyPerspective()) {
		ApplyPerspective();
		return true;
	}

	if (submode != SubMode::Segment
		|| event.GetKeyCode() != WXK_DELETE || !controller)
		return false;

	auto const view = controller->CaptureView();
	if (!view.selected_id)
		return false;
	auto const selected_id = *view.selected_id;
	auto const* guide = FindGuide(view, selected_id);
	if (!guide) {
		// Stale selection must not swallow Video-context Delete hotkeys.
		controller->Select(std::nullopt);
		return false;
	}
	return controller->Remove(selected_id);
}

void VisualToolMeasure::DrawHandles(VideoOverlayDrawContext& context, VisualGuide const& guide) {
	auto const viewport = parent->GetVisualGuideViewport();
	if (!IsVisualGuideViewportMappable(viewport))
		return;

	auto const style = MakeOverlayStyle(line_color_primary_opt, highlight_color_primary_opt);
	auto const draw_handle = [&](VisualGuidePoint point) {
		auto const canvas_point = VisualGuideToCanvas(point, viewport);
		context.SetLineColour(style.outline_colour, 0.95f, 2);
		context.SetFillColour(style.outline_colour, 0.85f);
		context.DrawCircle(canvas_point, kHandleRadius + 1.0f);
		context.SetLineColour(style.highlight_colour, 1.0f, 1);
		context.SetFillColour(style.highlight_colour, 0.85f);
		context.DrawCircle(canvas_point, kHandleRadius);
	};

	draw_handle(guide.first);
	draw_handle(guide.second);
}

void VisualToolMeasure::DrawPerspective(VideoOverlayDrawContext& context) {
	auto const* target = perspective_creation_preview
		? &*perspective_creation_preview
		: (perspective_state.Target() ? &*perspective_state.Target() : nullptr);
	auto const& viewport = parent->GetVisualGuideViewport();
	if (!target || !IsVisualGuideViewportMappable(viewport))
		return;

	std::array<Vector2D, 5> outline;
	for (std::size_t index = 0; index < target->size(); ++index)
		outline[index] = VisualGuideToCanvas(ToGuidePoint((*target)[index]), viewport);
	outline.back() = outline.front();

	auto const preview_validation = perspective_creation_preview
		? std::optional<perspective::GeometryValidation>(
			perspective::ValidateQuad(*perspective_creation_preview))
		: std::nullopt;
	auto const& state_validation = perspective_state.Validation();
	bool const valid = preview_validation
		? static_cast<bool>(*preview_validation)
		: state_validation && static_cast<bool>(*state_validation);
	wxColour const line_colour = !valid
		? wxColour(210, 55, 65)
		: to_wx((perspective_state.IsEditable()
			? line_color_primary_opt : line_color_secondary_opt)->GetColor());
	wxColour const handle_colour = !valid
		? wxColour(235, 80, 85)
		: to_wx((perspective_state.IsEditable()
			? highlight_color_primary_opt : highlight_color_secondary_opt)->GetColor());
	wxColour const handle_outline(20, 20, 20);

	context.SetLineColour(handle_outline, 0.95f, 3);
	context.DrawLineStrip(outline.data(), outline.size());
	context.SetLineColour(line_colour, 1.0f, 1);
	context.DrawLineStrip(outline.data(), outline.size());
	if (!perspective_state.IsEditable())
		return;

	constexpr std::array<perspective::PerspectiveQuadHandle, 4> corner_handles {
		perspective::PerspectiveQuadHandle::TopLeft,
		perspective::PerspectiveQuadHandle::TopRight,
		perspective::PerspectiveQuadHandle::BottomRight,
		perspective::PerspectiveQuadHandle::BottomLeft,
	};
	auto const active_handle = perspective_state.ActiveHandle();
	for (std::size_t index = 0; index < target->size(); ++index) {
		float const radius = corner_handles[index] == active_handle
			? kHandleRadius + 1.0f : kHandleRadius;
		context.SetLineColour(handle_outline, 0.95f, 2);
		context.SetFillColour(handle_outline, 0.85f);
		context.DrawCircle(outline[index], radius + 1.0f);
		context.SetLineColour(handle_colour, 1.0f, 1);
		context.SetFillColour(handle_colour, 0.9f);
		context.DrawCircle(outline[index], radius);
	}

	if (auto const center = perspective::PerspectiveQuadArithmeticCenter(*target)) {
		auto const canvas_center = VisualGuideToCanvas(ToGuidePoint(*center), viewport);
		float const center_radius = active_handle == perspective::PerspectiveQuadHandle::Center
			? kHandleRadius + 2.0f : kHandleRadius + 1.0f;
		context.SetLineColour(handle_outline, 0.95f, 2);
		context.SetFillColour(handle_outline, 0.85f);
		context.DrawCircle(canvas_center, center_radius + 1.0f);
		context.SetLineColour(handle_colour, 1.0f, 1);
		context.SetFillColour(handle_colour, 0.9f);
		context.DrawCircle(canvas_center, center_radius);
	}
}

void VisualToolMeasure::DrawWithContext(VideoOverlayDrawContext& context) {
	if (submode == SubMode::PerspectiveQuad) {
		DrawPerspective(context);
		return;
	}

	if (preview_guide) {
		VisualGuideOverlay overlay;
		overlay.DrawGuide(
			context,
			parent->GetVisualGuideViewport(),
			*preview_guide,
			true,
			MakeOverlayStyle(line_color_primary_opt, highlight_color_primary_opt));
		DrawHandles(context, *preview_guide);
		return;
	}

	if (!controller)
		return;
	auto const view = controller->CaptureView();
	if (!view.selected_id)
		return;
	if (auto const* guide = FindGuide(view, *view.selected_id))
		DrawHandles(context, *guide);
}

void VisualToolMeasure::Draw() {
	LegacyVideoOverlayDrawContext context(gl, *text);
	DrawWithContext(context);
}

void VisualToolMeasure::DrawOverlay(VideoOverlayDrawContext& context) {
	DrawWithContext(context);
}

void VisualToolMeasure::OnSubTool(wxCommandEvent& event) {
	if (event.GetId() == segment_button) {
		SetSubMode(static_cast<int>(SubMode::Segment));
		return;
	}
	if (event.GetId() == perspective_button) {
		SetSubMode(static_cast<int>(SubMode::PerspectiveQuad));
		return;
	}
	if (event.GetId() == clear_button && submode == SubMode::Segment) {
		CancelInteraction();
		if (controller)
			controller->Clear();
		return;
	}
	if (event.GetId() == apply_button && submode == SubMode::PerspectiveQuad) {
		ApplyPerspective();
		return;
	}
	if (event.GetId() == fit_button && submode == SubMode::PerspectiveQuad) {
		fit_text_to_target = event.IsChecked();
		OPT_SET(kPerspectiveFitTextOption)->SetBool(fit_text_to_target);
		if (perspective_diagnostic_depends_on_solver_options) {
			perspective_diagnostic.clear();
			perspective_diagnostic_depends_on_solver_options = false;
			c->ShowStatus({});
		}
		UpdateToolbarState();
		parent->Render();
		return;
	}
	if (event.GetId() == fax_frz_only_button
		&& submode == SubMode::PerspectiveQuad) {
		fax_frz_only = event.IsChecked();
		OPT_SET(kPerspectiveFaxFrzOnlyOption)->SetBool(fax_frz_only);
		if (perspective_diagnostic_depends_on_solver_options) {
			perspective_diagnostic.clear();
			perspective_diagnostic_depends_on_solver_options = false;
			c->ShowStatus({});
		}
		UpdateToolbarState();
		parent->Render();
		return;
	}
	if (event.GetId() != reset_button || submode != SubMode::PerspectiveQuad)
		return;

	CancelPerspectiveInteraction(false);
	(void)perspective_state.UseCurrent();
	bool const had_diagnostic = !perspective_diagnostic.empty();
	perspective_diagnostic.clear();
	perspective_diagnostic_depends_on_solver_options = false;
	if (had_diagnostic)
		c->ShowStatus({});
	UpdateToolbarState();
	parent->Render();
}

void VisualToolMeasure::SetToolbar(wxToolBar *new_toolbar) {
	if (toolbar)
		toolbar->Unbind(wxEVT_TOOL, &VisualToolMeasure::OnSubTool, this);
	toolbar = new_toolbar;
	if (!toolbar)
		return;

	int const icon_size = GetVideoUiIconSize(toolbar, OPT_GET("App/Toolbar Icon Size")->GetInt());
	toolbar->SetToolBitmapSize(wxSize(icon_size, icon_size));
	toolbar->AddSeparator();
	segment_button = toolbar->AddTool(
		wxID_ANY, _("Segment"),
		wxBitmapBundle::FromBitmap(CMD_ICON_GET(visual_vector_clip_line, wxLayout_Default, icon_size)),
		_("Measure a line segment"), wxITEM_CHECK)->GetId();
	perspective_button = toolbar->AddTool(
		wxID_ANY, _("Perspective Quad"),
		wxBitmapBundle::FromBitmap(CMD_ICON_GET(visual_vector_clip_drag, wxLayout_Default, icon_size)),
		_("Draw a target quadrilateral for the active subtitle"), wxITEM_CHECK)->GetId();
	toolbar->AddSeparator();
	clear_button = toolbar->AddTool(
		wxID_ANY, _("Clear all guides"),
		wxBitmapBundle::FromBitmap(CMD_ICON_GET(delete_button, wxLayout_Default, icon_size)),
		_("Remove all visual guides"))->GetId();
	reset_button = toolbar->AddTool(
		wxID_ANY, _("Use Current Subtitle Quad"),
		wxBitmapBundle::FromBitmap(CMD_ICON_GET(undo_button, wxLayout_Default, icon_size)),
		_("Initialize the target from the current subtitle geometry"))->GetId();
	fit_button = toolbar->AddTool(
		wxID_ANY, _("Fit Text"),
		wxBitmapBundle::FromBitmap(CMD_ICON_GET(visual_scale, wxLayout_Default, icon_size)),
		_("Scale text to fit the target quadrilateral"),
		wxITEM_CHECK)->GetId();
	fax_frz_only_button = toolbar->AddTool(
		wxID_ANY, _("Fax + Frz Only"),
		wxBitmapBundle::FromBitmap(CMD_ICON_GET(visual_rotatez, wxLayout_Default, icon_size)),
		_("Allow position and scale changes, but restrict Perspective to fax and frz"),
		wxITEM_CHECK)->GetId();
	apply_button = toolbar->AddTool(
		wxID_ANY, _("Apply Perspective Quad"),
		wxBitmapBundle::FromBitmap(CMD_ICON_GET(button_audio_commit, wxLayout_Default, icon_size)),
		_("Apply Perspective tags"))->GetId();
	toolbar->Realize();
	toolbar->Show(true);
	toolbar->Bind(wxEVT_TOOL, &VisualToolMeasure::OnSubTool, this);
	UpdateToolbarState();
}

bool VisualToolMeasure::SetSubMode(int mode) {
	if (mode < static_cast<int>(SubMode::Segment)
		|| mode > static_cast<int>(SubMode::PerspectiveQuad))
		return false;

	auto const new_mode = static_cast<SubMode>(mode);
	if (new_mode == submode) {
		UpdateToolbarState();
		return true;
	}

	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
	perspective_state.Clear();
	perspective_source.reset();
	perspective_diagnostic.clear();
	perspective_diagnostic_depends_on_solver_options = false;
	submode = new_mode;
	if (submode == SubMode::PerspectiveQuad)
		RebindPerspective(PerspectiveRebindMode::DiscardTarget, true);
	else
		UpdateToolbarState();
	parent->Render();
	return true;
}

void VisualToolMeasure::UpdateToolbarState() {
	if (!toolbar)
		return;

	if (segment_button >= 0)
		toolbar->ToggleTool(segment_button, submode == SubMode::Segment);
	if (perspective_button >= 0)
		toolbar->ToggleTool(perspective_button, submode == SubMode::PerspectiveQuad);
	std::string diagnostic = perspective_diagnostic;
	if (auto const& validation = perspective_state.Validation();
		validation && !*validation)
		diagnostic = perspective::DescribeGeometryError(validation->error);
	if (perspective_button >= 0)
		toolbar->SetToolShortHelp(perspective_button,
			diagnostic.empty()
				? _("Drag empty video space to create a target; drag corners to adjust it")
				: to_wx(diagnostic));
	if (clear_button >= 0)
		toolbar->EnableTool(clear_button, submode == SubMode::Segment && controller != nullptr);
	if (reset_button >= 0)
		toolbar->EnableTool(reset_button,
			submode == SubMode::PerspectiveQuad
			&& perspective_state.CanInitializeFromCurrent()
			&& !IsPerspectiveInteractionActive());
	if (fit_button >= 0) {
		toolbar->ToggleTool(fit_button, fit_text_to_target);
		toolbar->EnableTool(fit_button, submode == SubMode::PerspectiveQuad);
	}
	if (fax_frz_only_button >= 0) {
		toolbar->ToggleTool(fax_frz_only_button, fax_frz_only);
		toolbar->EnableTool(
			fax_frz_only_button, submode == SubMode::PerspectiveQuad);
	}
	if (apply_button >= 0) {
		toolbar->EnableTool(apply_button, CanApplyPerspective());
		toolbar->SetToolShortHelp(apply_button,
			diagnostic.empty() ? _("Apply Perspective tags") : to_wx(diagnostic));
	}
}

void VisualToolMeasure::OnFileChanged() {
	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
	if (submode != SubMode::PerspectiveQuad)
		return;

	if (command_session.IsLocalCommitInProgress()) {
		RebindPerspective(PerspectiveRebindMode::AcceptCurrent);
		return;
	}

	bool preserve_target = false;
	if (perspective_source && active_line) {
		// A target may outlive content changes, but never a file or line object
		// replacement: the recaptured fingerprint must still identify this source.
		auto core = c->GetCore();
		auto const& fingerprint = perspective_source->fingerprint;
		preserve_target = fingerprint.file_identity
				== reinterpret_cast<std::uintptr_t>(core.ass.get())
			&& fingerprint.line_identity
				== reinterpret_cast<std::uintptr_t>(active_line)
			&& fingerprint.line.id == active_line->Id;
	}
	RebindPerspective(preserve_target
		? PerspectiveRebindMode::PreserveTarget
		: PerspectiveRebindMode::DiscardTarget);
}

void VisualToolMeasure::OnFrameChanged() {
	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
	if (submode == SubMode::PerspectiveQuad)
		RebindPerspective();
}

void VisualToolMeasure::OnLineChanged() {
	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
	if (submode == SubMode::PerspectiveQuad)
		RebindPerspective();
}

void VisualToolMeasure::OnCoordinateSystemsChanged() {
	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
	if (submode == SubMode::PerspectiveQuad)
		RebindPerspective();
}

void VisualToolMeasure::OnDisplayAreaChanged() {
	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
}

void VisualToolMeasure::OnMouseCaptureLost(wxMouseCaptureLostEvent& event) {
	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
	VisualToolBase::OnMouseCaptureLost(event);
}
