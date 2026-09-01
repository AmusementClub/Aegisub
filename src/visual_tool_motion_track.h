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
	/// current edit: the riding sample's transform while a posed box is being
	/// edited (similarity matrix, or a pure translation for Translation
	/// tracks), identity only for untracked axis-aligned editing. Mouse
	/// points are transformed into seed space, so the drag/resize math works
	/// unchanged; the drawn box is transformed back, so the user grabs and
	/// drags the displayed rectangle/quad itself.
	struct EditPose {
		float m00 = 1.f, m01 = 0.f, m10 = 0.f, m11 = 1.f;
		float obj_cx = 0.f, obj_cy = 0.f;   // storage-space anchor
		float seed_cx = 0.f, seed_cy = 0.f; // matching seed-space anchor
		bool identity = true;
	};

	private:
	agi::Context *context;

	/// What the currently pressed left button is doing to the ROI.
	enum class RoiGrab {
		None, ///< no drag in progress
		Move, ///< pressed inside: translate, sides unchanged
		Band, ///< pressed outside: draw a fresh rectangle
		Nw,
		N,
		Ne,
		E,
		Se,
		S,
		Sw,
		W ///< pressed on that corner/edge grip: resize
	};

	RoiGrab grab = RoiGrab::None;
	Vector2D drag_offset; ///< Move: grab point relative to the ROI corner
	Vector2D band_start;  ///< fixed corner of an in-progress band drag
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
	/// The dialog's ROI coordinate-space anchor; see RoiRideAnchor.
	[[nodiscard]] aegisub::motion_track::RoiRideAnchor RoiAnchor() const;
	[[nodiscard]] bool StorageDims(int& width, int& height) const;
	void DrawWith(VideoOverlayDrawContext& draw);
	[[nodiscard]] Vector2D MouseToStorage(wxMouseEvent const& event) const;

	/// Classify a press at storage point `p` against `roi`: corner/edge grips
	/// first, then the interior (Move), else None. Tolerances are per-axis
	/// storage units so grips stay grabbable at every zoom level.
	[[nodiscard]] RoiGrab HitTest(Vector2D p, aegisub::motion_track::RoiRect roi,
								  float tol_x, float tol_y) const;
	static bool IsResize(RoiGrab grab);

	/// Normalize `roi` against the dialog's accepted side lengths, write it
	/// into the dialog's spin controls and request a repaint. `reanchor` is
	/// forwarded to DialogMotionTrack::SetOverlayRoi: false when the rectangle
	/// is expressed in the existing anchor's space (pose-mediated edits), true
	/// when it is storage coordinates at the presented frame (band drawing,
	/// identity-pose edits). `anchor_space` selects the clamp space: false
	/// clamps x/y into the frame and caps the sides at the storage dimensions;
	/// true leaves x/y untouched -- the pose maps them outside the frame
	/// whenever the tracked object left it -- and bounds only the sides at the
	/// dialog's accepted range, since pose scaling makes the storage size
	/// meaningless as an upper bound. Analyze clamps the final ROI in storage
	/// space after MapRoiToFrame rides it onto the seed frame.
	void PushRoi(aegisub::motion_track::RoiRect roi, bool reanchor = true,
				 bool anchor_space = false);

	/// Clear both drag flags and give the mouse back.
	void EndRoiDrag();

	void OnMouseCaptureLost(wxMouseCaptureLostEvent& event) override;

	public:
	VisualToolMotionTrack(VideoDisplay *parent, agi::Context *context);
	/// Out of line because gl_text points at a type this header only forward
	/// declares.
	~VisualToolMotionTrack();

	void OnMouseEvent(wxMouseEvent& event) override;
	bool OnKeyDown(wxKeyEvent& event) override { return false; }
	void Draw() override;
	bool SupportsOverlayContext() const override { return true; }
	void DrawOverlay(VideoOverlayDrawContext& context) override;
};
