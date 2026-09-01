#pragma once

#include "perspective_apply_plan.h"
#include "perspective_quad_edit_state.h"
#include "visual_guide_controller.h"
#include "visual_tool.h"

#include <memory>
#include <optional>
#include <string>

class OpenGLText;
class VideoOverlayDrawContext;
class wxCommandEvent;
class wxKeyEvent;
class wxMouseCaptureLostEvent;
class wxMouseEvent;
class wxToolBar;

/// Interactive editor for temporary visual measurement guides and the active
/// line's Perspective quad. Segment guides never touch subtitle data;
/// Perspective changes are committed only by the explicit Apply command.
class VisualToolMeasure final : public VisualToolBase {
	enum class SubMode {
		Segment,
		PerspectiveQuad,
	};

	enum class Interaction {
		None,
		CreatingMeasurement,
		DraggingFirstEndpoint,
		DraggingSecondEndpoint,
		DraggingLine,
	};
	enum class PerspectiveRebindMode {
		DiscardTarget,
		PreserveTarget,
		AcceptCurrent,
	};

	std::shared_ptr<VisualGuideController> controller;
	std::unique_ptr<OpenGLText> text;
	wxToolBar *toolbar = nullptr;
	SubMode submode = SubMode::Segment;
	Interaction interaction = Interaction::None;
	std::optional<std::string> editing_id;
	std::optional<VisualGuide> original_guide;
	std::optional<VisualGuide> preview_guide;
	VisualGuidePoint drag_start_point;
	perspective::PerspectiveQuadEditState perspective_state;
	std::optional<perspective::PerspectiveSourceSnapshot> perspective_source;
	std::optional<perspective::Vec2> perspective_creation_anchor;
	std::optional<Vector2D> perspective_creation_canvas_anchor;
	std::optional<perspective::Quad> perspective_creation_preview;
	std::string perspective_diagnostic;
	bool perspective_diagnostic_depends_on_solver_options = false;
	bool fit_text_to_target;
	bool fax_frz_only;

	int segment_button = -1;
	int perspective_button = -1;
	int clear_button = -1;
	int reset_button = -1;
	int fit_button = -1;
	int fax_frz_only_button = -1;
	int apply_button = -1;

	[[nodiscard]] std::optional<VisualGuidePoint> CurrentGuidePoint(bool clamp) const;
	[[nodiscard]] bool IsInsideVideoViewport(Vector2D point) const;

	void BeginInteraction(wxMouseEvent& event);
	void UpdateInteraction();
	void FinishInteraction();
	void CancelInteraction(bool render = true);
	[[nodiscard]] std::optional<perspective::Vec2> CurrentPerspectivePoint() const;
	void BeginPerspectiveInteraction();
	void UpdatePerspectiveInteraction();
	void FinishPerspectiveInteraction();
	void CancelPerspectiveInteraction(bool render = true);
	[[nodiscard]] bool IsPerspectiveInteractionActive() const noexcept;
	[[nodiscard]] std::optional<perspective::PerspectiveApplyContext>
		CurrentPerspectiveApplyContext() const;
	[[nodiscard]] bool CanApplyPerspective() const;
	void ApplyPerspective();
	void ClearPerspectiveBinding();
	void RebindPerspective(
		PerspectiveRebindMode mode = PerspectiveRebindMode::DiscardTarget,
		bool announce_diagnostic = false);
	void UpdateToolbarState();
	void DrawWithContext(VideoOverlayDrawContext& context);
	void DrawHandles(VideoOverlayDrawContext& context, VisualGuide const& guide);
	void DrawPerspective(VideoOverlayDrawContext& context);
	void OnSubTool(wxCommandEvent& event);
	bool ShouldRefreshOnAnyExternalCommit() const override {
		return submode == SubMode::PerspectiveQuad;
	}
	void OnFileChanged() override;
	void OnFrameChanged() override;
	void OnLineChanged() override;
	void OnCoordinateSystemsChanged() override;
	void OnDisplayAreaChanged() override;
	void OnMouseCaptureLost(wxMouseCaptureLostEvent& event) override;

public:
	VisualToolMeasure(VideoDisplay *parent, agi::Context *context);
	~VisualToolMeasure() override;

	void OnMouseEvent(wxMouseEvent& event) override;
	bool OnKeyDown(wxKeyEvent& event) override;
	void Draw() override;
	bool SupportsOverlayContext() const override { return true; }
	void DrawOverlay(VideoOverlayDrawContext& context) override;
	void SetToolbar(wxToolBar *toolbar) override;
	bool SetSubMode(int mode) override;
	int GetSubMode() const override { return static_cast<int>(submode); }
};
