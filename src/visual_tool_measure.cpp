#include "visual_tool_measure.h"

#include "compat.h"
#include "format.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "utils.h"
#include "video_display.h"
#include "video_overlay_draw_context.h"
#include "video_overlay_draw_context_legacy_gl.h"
#include "visual_guide_interaction.h"
#include "visual_guide_overlay.h"

#include <libaegisub/color.h>
#include <libaegisub/make_unique.h>

#include <algorithm>
#include <utility>

#include <wx/event.h>
#include <wx/toolbar.h>
#include <wx/translation.h>

namespace {
constexpr float kHandleRadius = 3.0f;
constexpr float kHitTolerance = 5.0f;

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
}

VisualToolMeasure::VisualToolMeasure(VideoDisplay *parent, agi::Context *context)
: VisualToolBase(parent, context)
, controller(context->GetUI().visualGuideController)
, text(agi::make_unique<OpenGLText>()) {
}

VisualToolMeasure::~VisualToolMeasure() {
	CancelInteraction(false);
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

void VisualToolMeasure::OnMouseEvent(wxMouseEvent& event) {
	shift_down = event.ShiftDown();
	ctrl_down = event.CmdDown();
	alt_down = event.AltDown();
	if (event.Leaving() && interaction == Interaction::None) {
		mouse_pos = Vector2D();
		parent->Render();
		return;
	}

	auto const point = event.GetPosition();
	mouse_pos = Vector2D(point.x, point.y);
	if (event.ButtonDown())
		parent->SetFocus();

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

	parent->Render();
}

bool VisualToolMeasure::OnKeyDown(wxKeyEvent& event) {
	if (event.GetKeyCode() == WXK_ESCAPE && interaction != Interaction::None) {
		CancelInteraction();
		return true;
	}

	if (event.GetKeyCode() != WXK_DELETE || !controller)
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

void VisualToolMeasure::DrawWithContext(VideoOverlayDrawContext& context) {
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
	if (event.GetId() != clear_button)
		return;

	CancelInteraction();
	if (controller)
		controller->Clear();
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
	clear_button = toolbar->AddTool(
		wxID_ANY, _("Clear all guides"),
		wxBitmapBundle::FromBitmap(CMD_ICON_GET(delete_button, wxLayout_Default, icon_size)),
		_("Remove all visual guides"))->GetId();
	toolbar->Realize();
	toolbar->Show(true);
	toolbar->Bind(wxEVT_TOOL, &VisualToolMeasure::OnSubTool, this);
}

void VisualToolMeasure::OnCoordinateSystemsChanged() {
	CancelInteraction();
}

void VisualToolMeasure::OnMouseCaptureLost(wxMouseCaptureLostEvent& event) {
	VisualToolBase::OnMouseCaptureLost(event);
	CancelInteraction();
}
