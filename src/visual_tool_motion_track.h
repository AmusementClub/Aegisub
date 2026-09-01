#pragma once

// Overlay visual tool for motion tracking: draws the published trajectory,
// failure markers and the current ROI rectangle; dragging the rectangle
// (panning, drawing anew, or resizing via corner/edge grips) updates the
// companion dialog's ROI fields. Requires the modeless dialog — without it
// there is nothing to draw and nothing to control.

#include "motion_track/session.h"
#include "visual_tool.h"

#include <memory>

class wxMouseEvent;
class wxKeyEvent;

class OpenGLText;
class VideoDisplay;

class VisualToolMotionTrack final : public VisualToolBase {
public:
	/// Pose mapping seed-space (dialog ROI) points to storage space for the
	/// current edit: the riding sample's similarity transform while a posed
	/// box is being edited, identity for plain axis-aligned editing. Mouse
	/// points are transformed into seed space, so the drag/resize math works
	/// unchanged; the drawn box is transformed back, so the user grabs and
	/// drags the rotated quad itself.
	struct EditPose {
		float m00 = 1.f, m01 = 0.f, m10 = 0.f, m11 = 1.f;
		float obj_cx = 0.f, obj_cy = 0.f;   // storage-space anchor
		float seed_cx = 0.f, seed_cy = 0.f; // matching seed-space anchor
		bool identity = true;
	};

private:
	agi::Context* context;

	/// What the currently pressed left button is doing to the ROI.
	enum class RoiGrab {
		None,   ///< no drag in progress
		Move,   ///< pressed inside: translate, sides unchanged
		Band,   ///< pressed outside: draw a fresh rectangle
		Nw, N, Ne, E, Se, S, Sw, W  ///< pressed on that corner/edge grip: resize
	};

	RoiGrab grab = RoiGrab::None;
	Vector2D drag_offset;        ///< Move: grab point relative to the ROI corner
	Vector2D band_start;         ///< fixed corner of an in-progress band drag
	/// Resize: seed-space edges that stay put; the edges named by
	/// `grab` follow the mouse instead.
	float fixed_left = 0.f, fixed_top = 0.f, fixed_right = 0.f,
	      fixed_bottom = 0.f;

	EditPose drag_pose;

	/// Only needed to satisfy the legacy GL context, which takes a text renderer
	/// by reference. This tool draws no text.
	std::unique_ptr<OpenGLText> gl_text;

	[[nodiscard]] std::shared_ptr<const aegisub::motion_track::MotionTrackSnapshot>
	SnapshotForOverlay() const;
	[[nodiscard]] bool CurrentRoi(aegisub::motion_track::RoiRect& roi) const;
	[[nodiscard]] bool StorageDims(int& width, int& height) const;
	void DrawWith(VideoOverlayDrawContext& draw);
	[[nodiscard]] Vector2D MouseToStorage(wxMouseEvent const& event) const;

	/// Classify a press at storage point `p` against `roi`: corner/edge grips
	/// first, then the interior (Move), else None. Tolerances are per-axis
	/// storage units so grips stay grabbable at every zoom level.
	[[nodiscard]] RoiGrab HitTest(Vector2D p, aegisub::motion_track::RoiRect roi,
	                              float tol_x, float tol_y) const;
	static bool IsResize(RoiGrab grab);

	/// Clamp `roi` to the frame and the dialog's accepted side lengths, write it
	/// into the dialog's spin controls and request a repaint.
	void PushRoi(aegisub::motion_track::RoiRect roi);

	/// Clear both drag flags and give the mouse back.
	void EndRoiDrag();

	void OnMouseCaptureLost(wxMouseCaptureLostEvent& event) override;

public:
	VisualToolMotionTrack(VideoDisplay* parent, agi::Context* context);
	/// Out of line because gl_text points at a type this header only forward
	/// declares.
	~VisualToolMotionTrack();

	void OnMouseEvent(wxMouseEvent& event) override;
	bool OnKeyDown(wxKeyEvent& event) override { return false; }
	void Draw() override;
	bool SupportsOverlayContext() const override { return true; }
	void DrawOverlay(VideoOverlayDrawContext& context) override;
};
