#pragma once

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

/// Interactive editor for temporary visual measurement guides. Persistent
/// guide drawing is owned by VisualGuideOverlay; this tool draws only its
/// handles and creation preview and never commits subtitle data.
class VisualToolMeasure final : public VisualToolBase {
	enum class Interaction {
		None,
		CreatingMeasurement,
		DraggingFirstEndpoint,
		DraggingSecondEndpoint,
		DraggingLine,
	};

	std::shared_ptr<VisualGuideController> controller;
	std::unique_ptr<OpenGLText> text;
	wxToolBar *toolbar = nullptr;
	Interaction interaction = Interaction::None;
	std::optional<std::string> editing_id;
	std::optional<VisualGuide> original_guide;
	std::optional<VisualGuide> preview_guide;
	VisualGuidePoint drag_start_point;

	int clear_button = -1;

	[[nodiscard]] std::optional<VisualGuidePoint> CurrentGuidePoint(bool clamp) const;
	[[nodiscard]] bool IsInsideVideoViewport(Vector2D point) const;

	void BeginInteraction(wxMouseEvent& event);
	void UpdateInteraction();
	void FinishInteraction();
	void CancelInteraction(bool render = true);
	void DrawWithContext(VideoOverlayDrawContext& context);
	void DrawHandles(VideoOverlayDrawContext& context, VisualGuide const& guide);
	void OnSubTool(wxCommandEvent& event);
	void OnCoordinateSystemsChanged() override;
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
};
