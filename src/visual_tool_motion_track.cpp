#include "visual_tool_motion_track.h"

#include "async_video_provider.h"
#include "compat.h"
#include "dialog_motion_track.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "dialog_manager.h"
#include "include/aegisub/context_ui.h"
#include "project.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_overlay_draw_context.h"
#include "video_overlay_draw_context_legacy_gl.h"
#include "visual_tool_roi_press_policy.h"

#include <libaegisub/make_unique.h>

#include <libaegisub/color.h>

#include <wx/event.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace aegisub::motion_track;

namespace {
// Hit tolerance for the resize grips, in display pixels; converted to storage
// units per axis at use sites so it holds at every zoom level.
constexpr float kHandleTolPx = 5.f;
// Half-size of a drawn grip square, in display pixels.
constexpr float kHandleHalfPx = 3.f;

// Clamp to the frame before measuring, not after: PushRoi can only push the
// box back inside, which would keep the out-of-frame part as width and give a
// bigger rectangle than the pointer ever covered.
Vector2D ClampToFrame(Vector2D v, int sw, int sh) {
	return Vector2D(std::clamp(v.X(), 0.f, float(sw)),
					std::clamp(v.Y(), 0.f, float(sh)));
}

Vector2D PoseToStorage(VisualToolMotionTrack::EditPose const& pose,
					   Vector2D seed_pt) {
	if (pose.identity)
		return seed_pt;
	float const dx = seed_pt.X() - pose.seed_cx;
	float const dy = seed_pt.Y() - pose.seed_cy;
	return Vector2D(pose.obj_cx + pose.m00 * dx + pose.m01 * dy,
					pose.obj_cy + pose.m10 * dx + pose.m11 * dy);
}

Vector2D PoseToSeed(VisualToolMotionTrack::EditPose const& pose,
					Vector2D storage_pt) {
	if (pose.identity)
		return storage_pt;
	float const dx = storage_pt.X() - pose.obj_cx;
	float const dy = storage_pt.Y() - pose.obj_cy;
	float const det = pose.m00 * pose.m11 - pose.m01 * pose.m10;
	if (std::abs(det) < 1e-9f)
		return storage_pt;
	return Vector2D(pose.seed_cx + (pose.m11 * dx - pose.m01 * dy) / det,
					pose.seed_cy + (-pose.m10 * dx + pose.m00 * dy) / det);
}

// Pose from the ride sample onto the anchor's frame: maps the ROI's
// coordinate space (the anchor frame, where the spin values were last
// expressed) to storage coordinates at the ride's frame. The linear part is
// the relative rotation/scale between the two frames; pure translation for
// the Translation model.
VisualToolMotionTrack::EditPose PoseFromAnchorRide(
	TrackSample const& ride, aegisub::motion_track::RoiRideAnchor const& anchor,
	TrackModel model) {
	VisualToolMotionTrack::EditPose pose;
	pose.identity = false;
	pose.obj_cx = float(ride.center_x);
	pose.obj_cy = float(ride.center_y);
	pose.seed_cx = float(anchor.center_x);
	pose.seed_cy = float(anchor.center_y);
	if (model == TrackModel::Similarity) {
		float const a00 = float(anchor.m00), a01 = float(anchor.m01);
		float const a10 = float(anchor.m10), a11 = float(anchor.m11);
		float const det = a00 * a11 - a01 * a10;
		if (std::abs(det) < 1e-9f)
			return pose;
		float const i00 = a11 / det, i01 = -a01 / det;
		float const i10 = -a10 / det, i11 = a00 / det;
		float const r00 = float(ride.transform.matrix[0]);
		float const r01 = float(ride.transform.matrix[1]);
		float const r10 = float(ride.transform.matrix[3]);
		float const r11 = float(ride.transform.matrix[4]);
		pose.m00 = r00 * i00 + r01 * i10;
		pose.m01 = r00 * i01 + r01 * i11;
		pose.m10 = r10 * i00 + r11 * i10;
		pose.m11 = r10 * i01 + r11 * i11;
	}
	return pose;
}
} // namespace

VisualToolMotionTrack::VisualToolMotionTrack(VideoDisplay *parent,
											 agi::Context *context)
	: VisualToolBase(parent, context), context(context), gl_text(agi::make_unique<OpenGLText>()) {}

// Out of line: OpenGLText is only forward-declared in the header, so the
// unique_ptr deleter needs the complete type here.
VisualToolMotionTrack::~VisualToolMotionTrack() = default;

std::shared_ptr<const MotionTrackSnapshot>
VisualToolMotionTrack::SnapshotForOverlay() const {
	auto *dialog = context->GetUI().dialog->Get<::DialogMotionTrack>();
	if (!dialog)
		return nullptr;
	return dialog->CaptureForOverlay();
}

bool VisualToolMotionTrack::CurrentRoi(RoiRect& roi) const {
	auto *dialog = context->GetUI().dialog->Get<::DialogMotionTrack>();
	if (!dialog)
		return false;
	roi = dialog->OverlayRoi();
	return true;
}

aegisub::motion_track::RoiRideAnchor VisualToolMotionTrack::RoiAnchor() const {
	auto *dialog = context->GetUI().dialog->Get<::DialogMotionTrack>();
	return dialog ? dialog->OverlayRoiAnchor()
				  : aegisub::motion_track::RoiRideAnchor{};
}

bool VisualToolMotionTrack::StorageDims(int& width, int& height) const {
	// Prefer the snapshot: a trajectory must be drawn against the dimensions it
	// was measured at, not whatever is loaded now.
	if (auto snap = SnapshotForOverlay();
		snap && snap->storage_width > 0 && snap->storage_height > 0) {
		width = snap->storage_width;
		height = snap->storage_height;
		return true;
	}
	// No run yet, so fall back to the live provider. Without this the overlay
	// stayed invisible until the first Analyze -- which is exactly when the ROI
	// still needs to be drawn on screen.
	if (auto *provider = context->GetCore().project->VideoProvider();
		provider && provider->GetWidth() > 0 && provider->GetHeight() > 0) {
		width = provider->GetWidth();
		height = provider->GetHeight();
		return true;
	}
	return false;
}

Vector2D VisualToolMotionTrack::MouseToStorage(
	wxMouseEvent const& event) const {
	int sw = 0, sh = 0;
	if (!StorageDims(sw, sh))
		return Vector2D();
	// video_res is the display-area size, so display -> storage scales by
	// storage/video_res (same direction as VisualToolBase::ToScriptCoords).
	double const sx = video_res.X() > 0 ? double(sw) / video_res.X() : 0.0;
	double const sy = video_res.Y() > 0 ? double(sh) / video_res.Y() : 0.0;
	wxPoint const mpos = event.GetPosition();
	Vector2D const mouse(float(mpos.x), float(mpos.y));
	return Vector2D(float((mouse.X() - video_pos.X()) * float(sx)),
					float((mouse.Y() - video_pos.Y()) * float(sy)));
}

void VisualToolMotionTrack::PushRoi(RoiRect roi, bool reanchor,
									bool anchor_space) {
	int sw = 0, sh = 0;
	if (!StorageDims(sw, sh))
		return;
	if (sw < ::DialogMotionTrack::kMinRoiSide || sh < ::DialogMotionTrack::kMinRoiSide)
		return;

	if (anchor_space) {
		// The rectangle lives in a pose's anchor space, so clamping x/y to the
		// frame would pin the box in place as soon as the pose translated it
		// off the seed position. Analyze re-clamps in storage space after
		// MapRoiToFrame rides the ROI onto the seed frame, so the push accepts
		// the full anchor range. The sides still keep the dialog's accepted
		// range, but without the storage-dimension cap: pose scaling maps
		// anchor lengths onto storage, so the storage size is not an upper
		// bound in this space.
		roi.w = std::clamp(roi.w, ::DialogMotionTrack::kMinRoiSide,
						   ::DialogMotionTrack::kMaxRoiSide);
		roi.h = std::clamp(roi.h, ::DialogMotionTrack::kMinRoiSide,
						   ::DialogMotionTrack::kMaxRoiSide);
	}
	else {
		// The dialog's spin controls define the accepted range; clamping here
		// keeps a drag that leaves the frame from being silently truncated to
		// a different rectangle than the one drawn.
		int const max_w = std::min(::DialogMotionTrack::kMaxRoiSide, sw);
		int const max_h = std::min(::DialogMotionTrack::kMaxRoiSide, sh);
		roi.w = std::clamp(roi.w, ::DialogMotionTrack::kMinRoiSide, max_w);
		roi.h = std::clamp(roi.h, ::DialogMotionTrack::kMinRoiSide, max_h);
		roi.x = std::clamp(roi.x, 0, sw - roi.w);
		roi.y = std::clamp(roi.y, 0, sh - roi.h);
	}

	if (auto *dialog = context->GetUI().dialog->Get<::DialogMotionTrack>()) {
		dialog->SetOverlayRoi(roi, reanchor);
		// Nothing else invalidates the display while a drag is in progress, so
		// without this the rectangle only catches up on the next unrelated
		// repaint.
		parent->Render();
	}
}

bool VisualToolMotionTrack::IsResize(RoiGrab grab) {
	switch (grab) {
		case RoiGrab::Nw:
		case RoiGrab::N:
		case RoiGrab::Ne:
		case RoiGrab::E:
		case RoiGrab::Se:
		case RoiGrab::S:
		case RoiGrab::Sw:
		case RoiGrab::W:
			return true;
		default:
			return false;
	}
}

VisualToolMotionTrack::RoiGrab VisualToolMotionTrack::HitTest(
	Vector2D p, RoiRect roi, float tol_x, float tol_y) const {
	float const l = float(roi.x), t = float(roi.y);
	float const r = float(roi.x + roi.w), b = float(roi.y + roi.h);

	bool const near_l = std::abs(p.X() - l) <= tol_x;
	bool const near_r = std::abs(p.X() - r) <= tol_x;
	bool const near_t = std::abs(p.Y() - t) <= tol_y;
	bool const near_b = std::abs(p.Y() - b) <= tol_y;
	bool const span_x = p.X() >= l && p.X() <= r;
	bool const span_y = p.Y() >= t && p.Y() <= b;

	// Corners win over edges so the diagonal grips survive on boxes smaller
	// than the tolerance; edges win over the interior so grabbing the border
	// resizes rather than pans.
	if (near_t && near_l)
		return RoiGrab::Nw;
	if (near_t && near_r)
		return RoiGrab::Ne;
	if (near_b && near_l)
		return RoiGrab::Sw;
	if (near_b && near_r)
		return RoiGrab::Se;
	if (near_t && span_x)
		return RoiGrab::N;
	if (near_b && span_x)
		return RoiGrab::S;
	if (near_l && span_y)
		return RoiGrab::W;
	if (near_r && span_y)
		return RoiGrab::E;
	if (span_x && span_y)
		return RoiGrab::Move;
	return RoiGrab::None;
}

void VisualToolMotionTrack::OnMouseEvent(wxMouseEvent& event) {
	RoiRect roi;
	bool const have_roi = CurrentRoi(roi);
	int sw = 0, sh = 0;
	bool const have_dims = StorageDims(sw, sh);

	// Losing the video mid-drag makes MouseToStorage return the invalid
	// sentinel, so stop rather than feeding garbage coordinates forward.
	if (!have_dims && grab != RoiGrab::None)
		EndRoiDrag();

	if (event.LeftDown() && have_dims) {
		// Capture the edit pose: the ride's affine relative to the ROI's
		// anchor when the pressed box rides a tracked frame, identity
		// otherwise. A band (drawing anew) always stays axis-aligned — it is
		// a fresh seed.
		drag_pose = EditPose{};
		auto snap = SnapshotForOverlay();
		if (snap && !snap->samples.empty() && have_roi) {
			int const current_frame =
				context->GetCore().videoController->GetPresentedFrameN();
			if (current_frame >= snap->samples.front().frame && current_frame <= snap->samples.back().frame) {
				if (auto const *s = FindSample(snap->samples, current_frame);
					s && s->status == TrackStatus::Ok) {
					if (auto const anchor = RoiAnchor(); anchor.valid)
						drag_pose = PoseFromAnchorRide(*s, anchor, snap->model);
				}
			}
		}

		// Hit testing and dragging run in the ROI's anchor space, where the
		// dialog ROI is axis-aligned even when the on-screen quad is rotated.
		Vector2D const press = MouseToStorage(event);
		Vector2D const p = PoseToSeed(drag_pose, press);
		float const tol_x = video_res.X() > 0
								? kHandleTolPx * float(sw) / float(video_res.X())
								: 0.f;
		float const tol_y = video_res.Y() > 0
								? kHandleTolPx * float(sh) / float(video_res.Y())
								: 0.f;
		RoiGrab const hit =
			have_roi ? HitTest(p, roi, tol_x, tol_y) : RoiGrab::None;
		// The frame test is on the untransformed storage point: the pose maps
		// into anchor space, where an ROI riding off the frame is expected to
		// sit outside it, so testing `p` would reject legitimate presses.
		bool const on_frame = visual_tool_roi_press_policy::PressIsOnFrame(
			press.X(), press.Y(), sw, sh);
		if (IsResize(hit)) {
			grab = hit;
			fixed_left = float(roi.x);
			fixed_top = float(roi.y);
			fixed_right = float(roi.x + roi.w);
			fixed_bottom = float(roi.y + roi.h);
		}
		else if (hit == RoiGrab::Move) {
			// Grab inside the box moves it; grab on a grip resizes it; grab
			// anywhere else draws a new one.
			grab = RoiGrab::Move;
			drag_offset = Vector2D(p.X() - roi.x, p.Y() - roi.y);
		}
		else if (visual_tool_roi_press_policy::PressMayStartBand(on_frame)) {
			grab = RoiGrab::Band;
			drag_pose.identity = true;
			band_start = press;
			// Commit to the new box straight away: a click with no drag should
			// still land a rectangle at the minimum size rather than nothing.
			PushRoi(RoiRect{int(std::lround(band_start.X())),
							int(std::lround(band_start.Y())), 0, 0},
					/*reanchor=*/true, /*anchor_space=*/false);
		}
		else {
			// A press on a letterbox bar that grabbed nothing: leave the ROI
			// and its anchor exactly as they were, and take no capture, so the
			// display keeps its usual handling of the click.
			event.Skip();
			return;
		}
		// Keep receiving motion after the pointer leaves the display, so a drag
		// towards the frame edge does not freeze half-finished.
		if (!parent->HasCapture())
			parent->CaptureMouse();
		event.Skip(false);
		return;
	}

	if (event.Dragging() && grab == RoiGrab::Band) {
		Vector2D const p = MouseToStorage(event);
		Vector2D const a = ClampToFrame(band_start, sw, sh);
		Vector2D const b = ClampToFrame(p, sw, sh);
		// Anchor stays put and either corner may lead, so normalise instead of
		// assuming the drag runs down-right.
		int const x0 = int(std::lround(std::min(a.X(), b.X())));
		int const y0 = int(std::lround(std::min(a.Y(), b.Y())));
		int const x1 = int(std::lround(std::max(a.X(), b.X())));
		int const y1 = int(std::lround(std::max(a.Y(), b.Y())));
		PushRoi(RoiRect{x0, y0, x1 - x0, y1 - y0},
				/*reanchor=*/true, /*anchor_space=*/false);
		event.Skip(false);
		return;
	}

	if (event.Dragging() && grab == RoiGrab::Move && have_roi) {
		Vector2D const p = PoseToSeed(drag_pose, MouseToStorage(event));
		// The pushed rectangle is in the anchor's space while a pose is
		// active, plain storage coordinates otherwise; PushRoi clamps it in
		// the matching space.
		PushRoi(RoiRect{int(std::lround(p.X() - drag_offset.X())),
						int(std::lround(p.Y() - drag_offset.Y())), roi.w,
						roi.h},
				/*reanchor=*/drag_pose.identity,
				/*anchor_space=*/!drag_pose.identity);
		event.Skip(false);
		return;
	}

	if (event.Dragging() && IsResize(grab)) {
		// Anchor-space point: the posed grips map onto the axis-aligned dialog
		// ROI. With a pose active the rectangle lives in that space, so PushRoi
		// must not clamp it to the frame (anchor_space below); an identity pose
		// yields plain storage coordinates and clamps as before.
		Vector2D const p = PoseToSeed(drag_pose, MouseToStorage(event));
		bool const follow_l =
			grab == RoiGrab::Nw || grab == RoiGrab::W || grab == RoiGrab::Sw;
		bool const follow_r =
			grab == RoiGrab::Ne || grab == RoiGrab::E || grab == RoiGrab::Se;
		bool const follow_t =
			grab == RoiGrab::Nw || grab == RoiGrab::N || grab == RoiGrab::Ne;
		bool const follow_b =
			grab == RoiGrab::Sw || grab == RoiGrab::S || grab == RoiGrab::Se;
		float const l = follow_l ? p.X() : fixed_left;
		float const r = follow_r ? p.X() : fixed_right;
		float const t = follow_t ? p.Y() : fixed_top;
		float const b = follow_b ? p.Y() : fixed_bottom;
		// The pointer may cross a fixed edge mid-drag, so normalise instead of
		// assuming which side leads; PushRoi enforces the minimum side from
		// there, which grows the box back towards the mouse.
		int const x0 = int(std::lround(std::min(l, r)));
		int const x1 = int(std::lround(std::max(l, r)));
		int const y0 = int(std::lround(std::min(t, b)));
		int const y1 = int(std::lround(std::max(t, b)));
		PushRoi(RoiRect{x0, y0, x1 - x0, y1 - y0},
				/*reanchor=*/drag_pose.identity,
				/*anchor_space=*/!drag_pose.identity);
		event.Skip(false);
		return;
	}

	if (event.LeftUp() && grab != RoiGrab::None) {
		EndRoiDrag();
		event.Skip(false);
		return;
	}
	event.Skip();
}

void VisualToolMotionTrack::EndRoiDrag() {
	grab = RoiGrab::None;
	if (parent->HasCapture())
		parent->ReleaseMouse();
}

void VisualToolMotionTrack::OnMouseCaptureLost(wxMouseCaptureLostEvent& event) {
	// Alt-tab or a modal stealing focus mid-drag must not leave the tool
	// rewriting the ROI on every later mouse move.
	grab = RoiGrab::None;
	VisualToolBase::OnMouseCaptureLost(event);
}
void VisualToolMotionTrack::DrawWith(VideoOverlayDrawContext& draw) {
	int sw = 0, sh = 0;
	if (!StorageDims(sw, sh) || sw <= 0 || sh <= 0)
		return;
	// Drawing goes the other way from MouseToStorage: storage -> display
	// scales by video_res/storage (as in VisualToolBase::FromScriptCoords).
	double const scale_x = double(video_res.X()) / sw;
	double const scale_y = double(video_res.Y()) / sh;

	wxColour const line =
		to_wx(OPT_GET("Colour/Visual Tools/Lines Primary")->GetColor());
	draw.SetLineColour(line, 1.0f, 2);
	draw.SetFillColour(wxColour(0, 0, 0), 0.0f);

	auto snap = SnapshotForOverlay();
	// Trajectory and failure marks only make sense while the presented frame
	// is inside the tracked span; outside it they are noise over the video.
	int current_frame = -1;
	bool in_range = false;
	if (snap && !snap->samples.empty()) {
		current_frame = context->GetCore().videoController->GetPresentedFrameN();
		in_range = current_frame >= snap->samples.front().frame && current_frame <= snap->samples.back().frame;
	}

	// Ghost of the last built apply plan: what Apply would write, drawn under
	// everything else in the secondary tool colour.
	if (auto *dialog = context->GetUI().dialog->Get<::DialogMotionTrack>()) {
		if (auto plan = dialog->CapturePlanPreview();
			plan && !plan->paths.empty()) {
			wxColour const ghost =
				to_wx(OPT_GET("Colour/Visual Tools/Lines Secondary")->GetColor());
			draw.SetLineColour(ghost, 0.8f, 1);
			draw.SetFillColour(ghost, 0.8f);
			for (auto const& path : plan->paths) {
				std::vector<Vector2D> pts;
				pts.reserve(path.size());
				for (auto const& p : path)
					pts.emplace_back(
						float(video_pos.X() + p.x * scale_x),
						float(video_pos.Y() + p.y * scale_y));
				if (pts.size() >= 2)
					draw.DrawLineStrip(pts.data(), pts.size());
				for (auto const& p : pts)
					draw.DrawRectangle(
						Vector2D(p.X() - 2, p.Y() - 2),
						Vector2D(p.X() + 2, p.Y() + 2));
			}
		}
	}

	RoiRect roi;
	if (CurrentRoi(roi)) {
		// The box rides the trajectory at the presented frame when that frame
		// has an Ok sample and the ROI's anchor defines a ride mapping: the
		// pose maps the anchor space onto the tracked affine, so scrubbing
		// shows the tracking quality directly and the box keeps whatever
		// offset the user dragged it to. While a drag is in progress the box
		// shows the editing target under the drag's captured pose instead, so
		// the box visibly follows the gesture. Without an anchor (or outside
		// the tracked span) the dialog ROI is plain storage coordinates and
		// draws as-is -- the same rectangle Analyze would seed.
		TrackSample const *ride = nullptr;
		if (in_range) {
			auto const *s = FindSample(snap->samples, current_frame);
			if (s && s->status == TrackStatus::Ok)
				ride = s;
		}

		EditPose pose;
		if (grab != RoiGrab::None) {
			pose = drag_pose;
		}
		else if (ride) {
			if (auto const anchor = RoiAnchor(); anchor.valid)
				pose = PoseFromAnchorRide(*ride, anchor, snap->model);
		}

		// Corner + edge-midpoint grip positions in seed space — the same
		// points OnMouseEvent hit-tests.
		Vector2D const seed_pts[8] = {
			Vector2D(float(roi.x), float(roi.y)),
			Vector2D(roi.x + (roi.w - 1) / 2.0f, float(roi.y)),
			Vector2D(float(roi.x + roi.w), float(roi.y)),
			Vector2D(float(roi.x + roi.w), roi.y + (roi.h - 1) / 2.0f),
			Vector2D(float(roi.x + roi.w), float(roi.y + roi.h)),
			Vector2D(roi.x + (roi.w - 1) / 2.0f, float(roi.y + roi.h)),
			Vector2D(float(roi.x), float(roi.y + roi.h)),
			Vector2D(float(roi.x), roi.y + (roi.h - 1) / 2.0f)};

		Vector2D box_top_left;
		if (!pose.identity) {
			// Similarity pose: the dialog ROI carried by the transform —
			// a rotated quad in storage space.
			Vector2D pts[8];
			for (int i = 0; i < 8; ++i)
				pts[i] = Vector2D(
					float(video_pos.X() + PoseToStorage(pose, seed_pts[i]).X() * scale_x),
					float(video_pos.Y() + PoseToStorage(pose, seed_pts[i]).Y() * scale_y));
			Vector2D const quad[4] = {pts[0], pts[2], pts[4], pts[6]};
			draw.SetFillColour(wxColour(0, 0, 0), 0.0f);
			draw.DrawPolygon(quad, 4);
			draw.SetFillColour(line, 1.0f);
			for (auto const& p : pts)
				draw.DrawRectangle(
					Vector2D(p.X() - kHandleHalfPx, p.Y() - kHandleHalfPx),
					Vector2D(p.X() + kHandleHalfPx, p.Y() + kHandleHalfPx));
			box_top_left = pts[0];
		}
		else {
			// Identity pose: the raw dialog rectangle, exactly the storage
			// coordinates CommitAnalyzeRequest would seed.
			Vector2D const p1(float(video_pos.X() + roi.x * scale_x),
							  float(video_pos.Y() + roi.y * scale_y));
			Vector2D const p2(p1.X() + float(roi.w * scale_x),
							  p1.Y() + float(roi.h * scale_y));
			draw.SetFillColour(wxColour(0, 0, 0), 0.0f);
			draw.DrawRectangle(p1, p2);
			draw.SetFillColour(line, 1.0f);
			for (int i = 0; i < 8; ++i) {
				Vector2D const p(
					float(video_pos.X() + PoseToStorage(pose, seed_pts[i]).X() * scale_x),
					float(video_pos.Y() + PoseToStorage(pose, seed_pts[i]).Y() * scale_y));
				draw.DrawRectangle(
					Vector2D(p.X() - kHandleHalfPx, p.Y() - kHandleHalfPx),
					Vector2D(p.X() + kHandleHalfPx, p.Y() + kHandleHalfPx));
			}
			box_top_left = p1;
		}

		// Per-frame readout above the box: scrub feedback without opening
		// anything. Only meaningful inside the tracked span and not while
		// the box is being edited.
		if (grab == RoiGrab::None && in_range) {
			auto const *s = FindSample(snap->samples, current_frame);
			char label[32] = {0};
			if (s && s->status == TrackStatus::Ok)
				std::snprintf(label, sizeof(label), "ncc %.2f", s->confidence);
			else if (s && s->status == TrackStatus::Failed)
				std::snprintf(label, sizeof(label), "lost");
			if (label[0]) {
				VideoOverlayTextStyle style;
				style.colour = line;
				wxSize const extent = draw.MeasureText(label, style);
				draw.DrawText(label, int(box_top_left.X()),
							  int(box_top_left.Y()) - extent.GetY() - 4, style);
			}
		}
	}

	if (!snap || snap->samples.empty() || !in_range)
		return;

	// Already-tracked frames draw solid; future frames are dimmed, the usual
	// past/future split tracker UIs use while scrubbing.
	std::vector<Vector2D> past;
	std::vector<Vector2D> future;
	for (auto const& s : snap->samples) {
		if (s.status != TrackStatus::Ok)
			continue;
		Vector2D const pt(float(video_pos.X() + s.center_x * scale_x),
						  float(video_pos.Y() + s.center_y * scale_y));
		(s.frame <= current_frame ? past : future).push_back(pt);
	}
	if (past.size() == 1 && !future.empty())
		// At the very first tracked frame the solid side is a single point
		// and nothing would draw; extend it with the next sample so the
		// path stays visible there.
		past.push_back(future.front());
	if (past.size() >= 2)
		draw.DrawLineStrip(past.data(), past.size());
	if (future.size() >= 2) {
		draw.SetLineColour(line, 0.3f, 2);
		draw.DrawLineStrip(future.data(), future.size());
		draw.SetLineColour(line, 1.0f, 2);
	}

	for (auto const& s : snap->samples) {
		if (s.status != TrackStatus::Failed)
			continue;
		float const cx = float(video_pos.X() + s.center_x * scale_x);
		float const cy = float(video_pos.Y() + s.center_y * scale_y);
		constexpr float r = 5.f;
		draw.DrawLine(Vector2D(cx - r, cy - r), Vector2D(cx + r, cy + r));
		draw.DrawLine(Vector2D(cx - r, cy + r), Vector2D(cx + r, cy - r));
	}
}

void VisualToolMotionTrack::Draw() {
	// The legacy GL pass calls Draw(); only the Skia overlay pass calls
	// DrawOverlay(), and Skia is off by default. Leaving this empty made the ROI
	// box invisible in every default configuration.
	LegacyVideoOverlayDrawContext draw(gl, *gl_text);
	DrawWith(draw);
}

void VisualToolMotionTrack::DrawOverlay(
	VideoOverlayDrawContext& context_draw) {
	DrawWith(context_draw);
}
