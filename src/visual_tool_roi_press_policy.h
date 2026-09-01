#pragma once

// Where a left press on the video display is allowed to act on the motion
// track ROI. VideoDisplay forwards every press inside its client area,
// letterbox bars included, and the display area is only as tight as the
// aspect fit allows -- so a press that produced storage coordinates outside
// the frame is a press on a black bar, not on a video pixel.
//
// This matters because the press handler's fallback branch draws a *new*
// rectangle: an off-frame press used to land a degenerate box that the
// clamp then turned into the minimum side glued to the frame edge, throwing
// away whatever ROI the user had set up (and re-anchoring it besides). The
// press must not create geometry from a coordinate that names no pixel.
//
// Adjusting an ROI that already exists is a different question, so this
// policy keeps them apart. A tracked box can legitimately ride partly off
// the frame -- the ROI lives in anchor space, where x/y are deliberately
// left unclamped -- which puts its grips near or past the frame edge. A
// press there still identifies a grip through the ordinary hit test, and
// honouring it costs nothing: the drag paths clamp their own output. So the
// rule is that an off-frame press may adjust the existing ROI but may never
// create a new one.

namespace visual_tool_roi_press_policy {

/// True when a storage-space press names a pixel of the frame. The frame is
/// treated as the closed rectangle [0,w]x[0,h]: storage coordinates are
/// continuous (a display pixel maps to a fractional storage position), so
/// the last row and column would otherwise be unreachable at some zoom
/// levels. Degenerate dimensions accept nothing -- the caller has no frame
/// to hit.
[[nodiscard]] inline bool PressIsOnFrame(float x, float y, int frame_width,
                                         int frame_height) noexcept {
	if (frame_width <= 0 || frame_height <= 0)
		return false;
	return x >= 0.f && y >= 0.f && x <= float(frame_width)
		&& y <= float(frame_height);
}

/// True when a press that hit neither a grip nor the ROI interior may start
/// a fresh band. Only presses on the frame may; off the frame the press is
/// on a letterbox bar and must leave the existing ROI alone.
[[nodiscard]] inline bool PressMayStartBand(bool press_is_on_frame) noexcept {
	return press_is_on_frame;
}

} // namespace visual_tool_roi_press_policy
