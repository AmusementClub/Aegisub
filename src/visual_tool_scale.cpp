// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

/// @file visual_tool_scale.cpp
/// @brief X/Y scaling visual typesetting tool
/// @ingroup visual_ts

#include "visual_tool_scale.h"

#include "compat.h"
#include "options.h"
#include "video_overlay_draw_context.h"
#include "video_overlay_helpers.h"

#include <wx/colour.h>

VisualToolScale::VisualToolScale(VideoDisplay *parent, agi::Context *context)
: VisualTool<VisualDraggableFeature>(parent, context)
{
}

void VisualToolScale::Draw() {
	if (!active_line) return;

	// The length in pixels of the 100% zoom
	static const int base_len = 160;
	// The width of the y scale guide/height of the x scale guide
	static const int guide_size = 10;

	// Load colors from options
	wxColour line_color_primary = to_wx(line_color_primary_opt->GetColor());
	wxColour line_color_secondary = to_wx(line_color_secondary_opt->GetColor());
	wxColour highlight_color = to_wx(highlight_color_primary_opt->GetColor());

	// Ensure that the scaling UI is comfortably visible on screen
	Vector2D base_point = pos
		.Max(Vector2D(base_len / 2 + guide_size, base_len / 2 + guide_size))
		.Min(video_res - base_len / 2 - guide_size * 3);

	// Set the origin to the base point and apply the line's rotation
	gl.SetOrigin(base_point);
	float const perspective_z_scale = video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(script_res, layout_res);
	gl.SetRotation(rx, ry, rz, perspective_z_scale);

	Vector2D scale_half_length = scale * base_len / 200;
	float minor_dim_offset = base_len / 2 + guide_size * 1.5f;

	// The ends of the current scale amount lines
	Vector2D x_p1(minor_dim_offset, -scale_half_length.Y());
	Vector2D x_p2(minor_dim_offset, scale_half_length.Y());
	Vector2D y_p1(-scale_half_length.X(), minor_dim_offset);
	Vector2D y_p2(scale_half_length.X(), minor_dim_offset);

	// Current scale amount lines
	gl.SetLineColour(line_color_primary, 1.f, 2);
	gl.DrawLine(x_p1, x_p2);
	gl.DrawLine(y_p1, y_p2);

	// Fake features at the end of the lines
	gl.SetLineColour(line_color_secondary, 1.f, 1);
	gl.SetFillColour(highlight_color, 0.3f);
	gl.DrawCircle(x_p1, 4);
	gl.DrawCircle(x_p2, 4);
	gl.DrawCircle(y_p1, 4);
	gl.DrawCircle(y_p2, 4);

	// Draw the guides
	int half_len = base_len / 2;
	gl.SetLineColour(line_color_secondary, 1.f, 1);
	gl.DrawRectangle(Vector2D(half_len, -half_len), Vector2D(half_len + guide_size, half_len));
	gl.DrawRectangle(Vector2D(-half_len, half_len), Vector2D(half_len, half_len + guide_size));

	// Draw the feet
	gl.SetLineColour(line_color_secondary, 1.f, 2);
	gl.DrawLine(Vector2D(half_len + guide_size, -half_len), Vector2D(half_len + guide_size + guide_size / 2, -half_len));
	gl.DrawLine(Vector2D(half_len + guide_size, half_len), Vector2D(half_len + guide_size + guide_size / 2, half_len));
	gl.DrawLine(Vector2D(-half_len, half_len + guide_size), Vector2D(-half_len, half_len + guide_size + guide_size / 2));
	gl.DrawLine(Vector2D(half_len, half_len + guide_size), Vector2D(half_len, half_len + guide_size + guide_size / 2));

	gl.ResetTransform();
}

void VisualToolScale::DrawOverlay(VideoOverlayDrawContext &context) {
	if (!active_line) return;

	static const int base_len = 160;
	static const int guide_size = 10;

	wxColour const line_color_primary = to_wx(line_color_primary_opt->GetColor());
	wxColour const line_color_secondary = to_wx(line_color_secondary_opt->GetColor());
	wxColour const highlight_color = to_wx(highlight_color_primary_opt->GetColor());
	Vector2D const overlay_scale(100.0f, 100.0f);
	float const perspective_z_scale = video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(script_res, layout_res);

	Vector2D const base_point = pos
		.Max(Vector2D(base_len / 2 + guide_size, base_len / 2 + guide_size))
		.Min(video_res - base_len / 2 - guide_size * 3);

	Vector2D const scale_half_length = scale * base_len / 200;
	float const minor_dim_offset = base_len / 2 + guide_size * 1.5f;

	Vector2D const x_p1(minor_dim_offset, -scale_half_length.Y());
	Vector2D const x_p2(minor_dim_offset, scale_half_length.Y());
	Vector2D const y_p1(-scale_half_length.X(), minor_dim_offset);
	Vector2D const y_p2(scale_half_length.X(), minor_dim_offset);

	context.SetLineColour(line_color_primary, 1.f, 2);
	video_overlay_helpers::DrawProjectedLine(context, x_p1, x_p2, base_point, overlay_scale, rx, ry, rz, perspective_z_scale);
	video_overlay_helpers::DrawProjectedLine(context, y_p1, y_p2, base_point, overlay_scale, rx, ry, rz, perspective_z_scale);

	context.SetLineColour(line_color_secondary, 1.f, 1);
	context.SetFillColour(highlight_color, 0.3f);
	context.DrawCircle(
		video_overlay_helpers::ProjectScaledRotatedPoint(x_p1, base_point, overlay_scale, rx, ry, rz, perspective_z_scale),
		video_overlay_helpers::ProjectCircleRadius(x_p1, 4.0f, base_point, overlay_scale, rx, ry, rz, perspective_z_scale));
	context.DrawCircle(
		video_overlay_helpers::ProjectScaledRotatedPoint(x_p2, base_point, overlay_scale, rx, ry, rz, perspective_z_scale),
		video_overlay_helpers::ProjectCircleRadius(x_p2, 4.0f, base_point, overlay_scale, rx, ry, rz, perspective_z_scale));
	context.DrawCircle(
		video_overlay_helpers::ProjectScaledRotatedPoint(y_p1, base_point, overlay_scale, rx, ry, rz, perspective_z_scale),
		video_overlay_helpers::ProjectCircleRadius(y_p1, 4.0f, base_point, overlay_scale, rx, ry, rz, perspective_z_scale));
	context.DrawCircle(
		video_overlay_helpers::ProjectScaledRotatedPoint(y_p2, base_point, overlay_scale, rx, ry, rz, perspective_z_scale),
		video_overlay_helpers::ProjectCircleRadius(y_p2, 4.0f, base_point, overlay_scale, rx, ry, rz, perspective_z_scale));

	int const half_len = base_len / 2;
	context.SetLineColour(line_color_secondary, 1.0f, 1);
	context.SetFillColour(highlight_color, 0.3f);
	video_overlay_helpers::DrawProjectedQuad(
		context,
		Vector2D(half_len, -half_len),
		Vector2D(half_len + guide_size, -half_len),
		Vector2D(half_len + guide_size, half_len),
		Vector2D(half_len, half_len),
		base_point, overlay_scale, rx, ry, rz, perspective_z_scale);
	video_overlay_helpers::DrawProjectedQuad(
		context,
		Vector2D(-half_len, half_len),
		Vector2D(half_len, half_len),
		Vector2D(half_len, half_len + guide_size),
		Vector2D(-half_len, half_len + guide_size),
		base_point, overlay_scale, rx, ry, rz, perspective_z_scale);
	context.SetFillColour(highlight_color, 0.0f);

	context.SetLineColour(line_color_secondary, 1.f, 2);
	video_overlay_helpers::DrawProjectedLine(context, Vector2D(half_len + guide_size, -half_len), Vector2D(half_len + guide_size + guide_size / 2, -half_len), base_point, overlay_scale, rx, ry, rz, perspective_z_scale);
	video_overlay_helpers::DrawProjectedLine(context, Vector2D(half_len + guide_size, half_len), Vector2D(half_len + guide_size + guide_size / 2, half_len), base_point, overlay_scale, rx, ry, rz, perspective_z_scale);
	video_overlay_helpers::DrawProjectedLine(context, Vector2D(-half_len, half_len + guide_size), Vector2D(-half_len, half_len + guide_size + guide_size / 2), base_point, overlay_scale, rx, ry, rz, perspective_z_scale);
	video_overlay_helpers::DrawProjectedLine(context, Vector2D(half_len, half_len + guide_size), Vector2D(half_len, half_len + guide_size + guide_size / 2), base_point, overlay_scale, rx, ry, rz, perspective_z_scale);
}

bool VisualToolScale::InitializeHold() {
	initial_scale = scale;
	return true;
}

void VisualToolScale::UpdateHold() {
	Vector2D delta = (mouse_pos - drag_start) * Vector2D(1, -1);
	if (shift_down)
		delta = delta.SingleAxis();
	if (alt_down) {
		if (std::abs(delta.X()) > std::abs(delta.Y()))
			delta = Vector2D(delta.X(), delta.X() * (initial_scale.Y() / initial_scale.X()));
		else
			delta = Vector2D(delta.Y() * (initial_scale.X() / initial_scale.Y()), delta.Y());
	}

	scale = Vector2D(0, 0).Max(delta * 1.25f + initial_scale);
	if (ctrl_down)
		scale = scale.Round(25.f);

	SetSelectedOverride("\\fscx", std::to_string((int)scale.X()));
	SetSelectedOverride("\\fscy", std::to_string((int)scale.Y()));
}

void VisualToolScale::DoRefresh() {
	if (!active_line) return;

	GetLineScale(active_line, scale);
	GetLineRotation(active_line, rx, ry, rz);
	pos = FromScriptCoords(GetLinePosition(active_line));
}
