// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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

/// @file visual_tool.cpp
/// @brief Base class for visual typesetting functions
/// @ingroup visual_ts

#include "visual_tool.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_file_app.h"
#include "ass_style.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_overlay_draw_context.h"
#include "visual_tool_clip.h"
#include "visual_tool_drag.h"
#include "visual_tool_vector_clip.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/format.h>
#include <libaegisub/of_type_adaptor.h>

#include <algorithm>

VisualToolBase::VisualToolBase(VideoDisplay *parent, agi::Context *context)
: c(context)
, parent(parent)
, command_session(context->GetCore().ass.get())
, frame_number(c->GetCore().videoController->GetFrameN())
, highlight_color_primary_opt(OPT_GET("Colour/Visual Tools/Highlight Primary"))
, highlight_color_secondary_opt(OPT_GET("Colour/Visual Tools/Highlight Secondary"))
, line_color_primary_opt(OPT_GET("Colour/Visual Tools/Lines Primary"))
, line_color_secondary_opt(OPT_GET("Colour/Visual Tools/Lines Secondary"))
, shaded_area_alpha_opt(OPT_GET("Colour/Visual Tools/Shaded Area Alpha"))
, file_changed_connection(c->GetCore().ass->AddCommitListener(&VisualToolBase::OnCommit, this))
{
	auto core = c->GetCore();
	UpdateScriptResolution();
	active_line = GetActiveDialogueLine();
	connections.push_back(core.selectionController->AddActiveLineListener(&VisualToolBase::OnActiveLineChanged, this));
	connections.push_back(core.videoController->AddFramePresentedListener(&VisualToolBase::OnFramePresented, this));
	parent->Bind(wxEVT_MOUSE_CAPTURE_LOST, &VisualToolBase::OnMouseCaptureLost, this);
}

void VisualToolBase::UpdateScriptResolution() {
	int script_w, script_h;
	auto core = c->GetCore();
	core.ass->GetResolution(ScriptResolutionType::PlayRes, script_w, script_h);
	script_res = Vector2D(script_w, script_h);
}

void VisualToolBase::OnCommit(int type, AssDialogue const* changed) {
	bool const local_commit = command_session.IsLocalCommitInProgress();
	if (local_commit && !command_session.ShouldObserveLocalCommit())
		return;

	if (!local_commit) {
		holding = false;
		dragging = false;
	}

	auto *new_active_line = GetActiveDialogueLine();
	bool needs_render = false;
	bool const coordinate_system_changed = type == AssFile::COMMIT_NEW || type & AssFile::COMMIT_SCRIPTINFO;

	if (coordinate_system_changed) {
		active_line = new_active_line;
		UpdateScriptResolution();
		OnCoordinateSystemsChanged();
		needs_render = true;
	}

	bool needs_file_refresh = false;
	if (type & (AssFile::COMMIT_STYLES | AssFile::COMMIT_ORDER | AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_META | AssFile::COMMIT_DIAG_TIME))
		needs_file_refresh = true;
	else if (type & (AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_EXTRADATA)) {
		if (!changed)
			needs_file_refresh = true;
		else {
			needs_file_refresh = changed == active_line
				|| changed == new_active_line
				|| IsDisplayed(changed);
		}
	}

	if (needs_file_refresh) {
		active_line = new_active_line;
		OnFileChanged();
		needs_render = true;
	}

	if (needs_render)
		parent->Render();
}

void VisualToolBase::OnFramePresented(int new_frame) {
	if (frame_number == new_frame) return;

	frame_number = new_frame;
	OnFrameChanged();

	AssDialogue *new_line = GetActiveDialogueLine();
	if (new_line != active_line) {
		dragging = false;
		active_line = new_line;
		OnLineChanged();
	}
}

void VisualToolBase::OnMouseCaptureLost(wxMouseCaptureLostEvent &) {
	holding = false;
	dragging = false;
}

void VisualToolBase::OnActiveLineChanged(AssDialogue *new_line) {
	if (!IsDisplayed(new_line))
		new_line = nullptr;

	holding = false;
	dragging = false;
	if (new_line != active_line) {
		active_line = new_line;
		OnLineChanged();
		parent->Render();
	}
}

bool VisualToolBase::IsDisplayed(AssDialogue const* line) const {
	int frame = frame_number;
	if (frame < 0)
		frame = c->GetCore().videoController->GetFrameN();
	auto core = c->GetCore();
	return line
		&& !line->Comment
		&& core.videoController->FrameAtTime(line->Start, agi::vfr::START) <= frame
		&& core.videoController->FrameAtTime(line->End, agi::vfr::END) >= frame;
}

AssDialogue *VisualToolBase::GetCommitTargetLine() const {
	auto const& selected = c->GetCore().selectionController->GetSelectedSet();
	if (selected.size() == 1)
		return *selected.begin();
	return nullptr;
}

void VisualToolBase::Commit(wxString message) {
	if (message.empty())
		message = _("visual typesetting");

	command_session.Commit(from_wx(message), AssFile::COMMIT_DIAG_TEXT, command_session.GetCommitId(), GetCommitTargetLine());
}

void VisualToolBase::CommitAndRefresh(wxString message) {
	if (message.empty())
		message = _("visual typesetting");

	command_session.CommitWithFeedback(
		from_wx(message),
		AssFile::COMMIT_DIAG_TEXT,
		command_session.GetCommitId(),
		GetCommitTargetLine(),
		aegisub::LocalCommitFeedback::ObserveSelf);
}

AssDialogue* VisualToolBase::GetActiveDialogueLine() {
	auto core = c->GetCore();
	AssDialogue *diag = core.selectionController->GetActiveLine();
	if (IsDisplayed(diag))
		return diag;
	return nullptr;
}

void VisualToolBase::SetCanvasSize(int w, int h) {
	canvas_size = Vector2D(w, h);
}

void VisualToolBase::SetDisplayArea(int x, int y, int w, int h) {
	if (x == video_pos.X() && y == video_pos.Y() && w == video_res.X() && h == video_res.Y()) return;

	video_pos = Vector2D(x, y);
	video_res = Vector2D(w, h);

	holding = false;
	dragging = false;
	if (parent->HasCapture())
		parent->ReleaseMouse();
	OnCoordinateSystemsChanged();
}

Vector2D VisualToolBase::ToScriptCoords(Vector2D point) const {
	return (point - video_pos) * script_res / video_res;
}

Vector2D VisualToolBase::FromScriptCoords(Vector2D point) const {
	return (point * video_res / script_res) + video_pos;
}

template<class FeatureType>
VisualTool<FeatureType>::VisualTool(VideoDisplay *parent, agi::Context *context)
: VisualToolBase(parent, context)
{
}

template<class FeatureType>
void VisualTool<FeatureType>::OnMouseEvent(wxMouseEvent &event) {
	bool left_click = event.LeftDown();
	bool left_double = event.LeftDClick();
	shift_down = event.ShiftDown();
	ctrl_down = event.CmdDown();
	alt_down = event.AltDown();

	wxPoint pt = event.GetPosition(); mouse_pos = Vector2D(pt.x, pt.y);

	if (event.Leaving()) {
		mouse_pos = Vector2D();
		parent->RenderNow();
		return;
	}

	if (!dragging) {
		int max_layer = INT_MIN;
		active_feature = nullptr;
		for (auto& feature : features) {
			if (feature.IsMouseOver(mouse_pos) && feature.layer >= max_layer) {
				active_feature = &feature;
				max_layer = feature.layer;
			}
		}
	}

	if (dragging) {
		// continue drag
		if (event.LeftIsDown()) {
			for (auto sel : sel_features)
				sel->UpdateDrag(mouse_pos - drag_start, shift_down);
			for (auto sel : sel_features)
				UpdateDrag(sel);
			Commit();
		}
		// end drag
		else {
			dragging = false;

			// mouse didn't move, fiddle with selection
			if (active_feature && !active_feature->HasMoved()) {
				// Don't deselect stuff that was selected in this click's mousedown event
				if (!sel_changed) {
					if (ctrl_down)
						RemoveSelection(active_feature);
					else
						SetSelection(active_feature, true);
				}
			}

			active_feature = nullptr;
			parent->ReleaseMouse();
			parent->SetFocus();
		}
	}
	else if (holding) {
		if (!event.LeftIsDown()) {
			holding = false;

			parent->ReleaseMouse();
			parent->SetFocus();
		}

		UpdateHold();
		Commit();

	}
	else if (left_click) {
		drag_start = mouse_pos;
		auto core = c->GetCore();

		// start drag
		if (active_feature) {
			if (!sel_features.count(active_feature)) {
				sel_changed = true;
				SetSelection(active_feature, !ctrl_down);
			}
			else
				sel_changed = false;

			if (active_feature->line)
				core.selectionController->SetActiveLine(active_feature->line);

			if (InitializeDrag(active_feature)) {
				for (auto sel : sel_features) sel->StartDrag();
				dragging = true;
				parent->CaptureMouse();
			}
		}
		// start hold
		else {
			if (!alt_down && features.size() > 1) {
				sel_features.clear();
				core.selectionController->SetSelectedSet({ core.selectionController->GetActiveLine() });
			}
			if (active_line && InitializeHold()) {
				holding = true;
				parent->CaptureMouse();
			}
		}
	}

	if (active_line && left_double)
		OnDoubleClick();

	parent->RenderNow();

	// Only coalesce the changes made in a single drag
	if (!event.LeftIsDown())
		command_session.ResetCommitId();
}

template<class FeatureType>
void VisualTool<FeatureType>::DrawAllFeatures() {
	wxColour grid_color = to_wx(line_color_secondary_opt->GetColor());
	gl.SetLineColour(grid_color, 1.0f, 1);
	wxColour base_fill = to_wx(highlight_color_primary_opt->GetColor());
	wxColour active_fill = to_wx(highlight_color_secondary_opt->GetColor());
	wxColour alt_fill = to_wx(line_color_primary_opt->GetColor());
	for (auto& feature : features) {
		wxColour fill = base_fill;
		if (&feature == active_feature)
			fill = active_fill;
		else if (sel_features.count(&feature))
			fill = alt_fill;
		gl.SetFillColour(fill, 0.3f);
		feature.Draw(gl);
	}
}

template<class FeatureType>
void VisualTool<FeatureType>::DrawAllFeatures(VideoOverlayDrawContext &context) {
	wxColour const grid_color = to_wx(line_color_secondary_opt->GetColor());
	context.SetLineColour(grid_color, 1.0f, 1);
	wxColour const base_fill = to_wx(highlight_color_primary_opt->GetColor());
	wxColour const active_fill = to_wx(highlight_color_secondary_opt->GetColor());
	wxColour const alt_fill = to_wx(line_color_primary_opt->GetColor());
	for (auto& feature : features) {
		wxColour fill = base_fill;
		if (&feature == active_feature)
			fill = active_fill;
		else if (sel_features.count(&feature))
			fill = alt_fill;
		context.SetFillColour(fill, 0.3f);
		feature.Draw(context);
	}
}

template<class FeatureType>
void VisualTool<FeatureType>::SetSelection(FeatureType *feat, bool clear) {
	if (clear)
		sel_features.clear();

	if (sel_features.insert(feat).second && feat->line) {
		auto core = c->GetCore();
		Selection sel;
		if (!clear)
			sel = core.selectionController->GetSelectedSet();
		if (sel.insert(feat->line).second)
			core.selectionController->SetSelectedSet(std::move(sel));
	}
}

template<class FeatureType>
void VisualTool<FeatureType>::RemoveSelection(FeatureType *feat) {
	if (!sel_features.erase(feat) || !feat->line) return;
	for (auto sel : sel_features)
		if (sel->line == feat->line) return;

	auto core = c->GetCore();
	auto sel = core.selectionController->GetSelectedSet();

	// Don't deselect the only selected line
	if (sel.size() <= 1) return;

	sel.erase(feat->line);

	// Set the active line to an arbitrary selected line if we just
	// deselected the active line
	AssDialogue *new_active = core.selectionController->GetActiveLine();
	if (feat->line == new_active)
		new_active = *sel.begin();

	core.selectionController->SetSelectionAndActive(std::move(sel), new_active);
}

//////// PARSERS

typedef const std::vector<AssOverrideParameter> * param_vec;

// Find a tag's parameters in a line or return nullptr if it's not found
static param_vec find_tag(std::vector<std::unique_ptr<AssDialogueBlock>>& blocks, std::string const& tag_name) {
	for (auto ovr : blocks | agi::of_type<AssDialogueBlockOverride>()) {
		for (auto const& tag : ovr->Tags) {
			if (tag.Name == tag_name)
				return &tag.Params;
		}
	}

	return nullptr;
}

// Get a Vector2D from the given tag parameters, or Vector2D::Bad() if they are not valid
static Vector2D vec_or_bad(param_vec tag, size_t x_idx, size_t y_idx) {
	if (!tag ||
		tag->size() <= x_idx || tag->size() <= y_idx ||
		(*tag)[x_idx].omitted || (*tag)[y_idx].omitted)
	{
		return Vector2D();
	}
	return Vector2D((*tag)[x_idx].Get<float>(), (*tag)[y_idx].Get<float>());
}

Vector2D VisualToolBase::GetLinePosition(AssDialogue *diag) {
	auto blocks = diag->ParseTags();

	if (Vector2D ret = vec_or_bad(find_tag(blocks, "\\pos"), 0, 1)) return ret;
	if (Vector2D ret = vec_or_bad(find_tag(blocks, "\\move"), 0, 1)) return ret;

	// Get default position
	auto margin = diag->Margin;
	int align = 2;

	auto core = c->GetCore();
	if (AssStyle *style = core.ass->GetStyle(diag->Style)) {
		align = style->alignment;
		for (int i = 0; i < 3; i++) {
			if (margin[i] == 0)
				margin[i] = style->Margin[i];
		}
	}

	param_vec align_tag;
	int ovr_align = 0;
	if ((align_tag = find_tag(blocks, "\\an")))
		ovr_align = (*align_tag)[0].Get<int>(ovr_align);
	else if ((align_tag = find_tag(blocks, "\\a")))
		ovr_align = AssStyle::SsaToAss((*align_tag)[0].Get<int>(2));

	if (ovr_align > 0 && ovr_align <= 9)
		align = ovr_align;

	// Alignment type
	int hor = (align - 1) % 3;
	int vert = (align - 1) / 3;

	// Calculate positions
	int x, y;
	if (hor == 0)
		x = margin[0];
	else if (hor == 1)
		x = (script_res.X() + margin[0] - margin[1]) / 2;
	else
		x = script_res.X() - margin[1];

	if (vert == 0)
		y = script_res.Y() - margin[2];
	else if (vert == 1)
		y = script_res.Y() / 2;
	else
		y = margin[2];

	return Vector2D(x, y);
}

Vector2D VisualToolBase::GetLineOrigin(AssDialogue *diag) {
	auto blocks = diag->ParseTags();
	return vec_or_bad(find_tag(blocks, "\\org"), 0, 1);
}

bool VisualToolBase::GetLineMove(AssDialogue *diag, Vector2D &p1, Vector2D &p2, int &t1, int &t2) {
	auto blocks = diag->ParseTags();

	param_vec tag = find_tag(blocks, "\\move");
	if (!tag)
		return false;

	p1 = vec_or_bad(tag, 0, 1);
	p2 = vec_or_bad(tag, 2, 3);
	// VSFilter actually defaults to -1, but it uses <= 0 to check for default and 0 seems less bug-prone
	t1 = (*tag)[4].Get<int>(0);
	t2 = (*tag)[5].Get<int>(0);

	return p1 && p2;
}

void VisualToolBase::GetLineRotation(AssDialogue *diag, float &rx, float &ry, float &rz) {
	rx = ry = rz = 0.f;

	auto core = c->GetCore();
	if (AssStyle *style = core.ass->GetStyle(diag->Style))
		rz = style->angle;

	auto blocks = diag->ParseTags();

	if (param_vec tag = find_tag(blocks, "\\frx"))
		rx = tag->front().Get(rx);
	if (param_vec tag = find_tag(blocks, "\\fry"))
		ry = tag->front().Get(ry);
	if (param_vec tag = find_tag(blocks, "\\frz"))
		rz = tag->front().Get(rz);
	else if ((tag = find_tag(blocks, "\\fr")))
		rz = tag->front().Get(rz);
}

void VisualToolBase::GetLineShear(AssDialogue *diag, float& fax, float& fay) {
	fax = fay = 0.f;

	auto blocks = diag->ParseTags();

	if (param_vec tag = find_tag(blocks, "\\fax"))
		fax = tag->front().Get(fax);
	if (param_vec tag = find_tag(blocks, "\\fay"))
		fay = tag->front().Get(fay);
}

void VisualToolBase::GetLineScale(AssDialogue *diag, Vector2D &scale) {
	float x = 100.f, y = 100.f;

	auto core = c->GetCore();
	if (AssStyle *style = core.ass->GetStyle(diag->Style)) {
		x = style->scalex;
		y = style->scaley;
	}

	auto blocks = diag->ParseTags();

	if (param_vec tag = find_tag(blocks, "\\fscx"))
		x = tag->front().Get(x);
	if (param_vec tag = find_tag(blocks, "\\fscy"))
		y = tag->front().Get(y);

	scale = Vector2D(x, y);
}

void VisualToolBase::GetLineClip(AssDialogue *diag, Vector2D &p1, Vector2D &p2, bool &inverse) {
	inverse = false;

	auto blocks = diag->ParseTags();
	param_vec tag = find_tag(blocks, "\\iclip");
	if (tag)
		inverse = true;
	else
		tag = find_tag(blocks, "\\clip");

	if (tag && tag->size() == 4) {
		p1 = vec_or_bad(tag, 0, 1);
		p2 = vec_or_bad(tag, 2, 3);
	}
	else {
		p1 = Vector2D(0, 0);
		p2 = script_res - 1;
	}
}

std::string VisualToolBase::GetLineVectorClip(AssDialogue *diag, int &scale, bool &inverse) {
	auto blocks = diag->ParseTags();

	scale = 1;
	inverse = false;

	param_vec tag = find_tag(blocks, "\\iclip");
	if (tag)
		inverse = true;
	else
		tag = find_tag(blocks, "\\clip");

	if (tag && tag->size() == 4) {
		return agi::format("m %d %d l %d %d %d %d %d %d"
			, (*tag)[0].Get<int>(), (*tag)[1].Get<int>()
			, (*tag)[2].Get<int>(), (*tag)[1].Get<int>()
			, (*tag)[2].Get<int>(), (*tag)[3].Get<int>()
			, (*tag)[0].Get<int>(), (*tag)[3].Get<int>());
	}
	if (tag) {
		scale = std::max((*tag)[0].Get(scale), 1);
		return (*tag)[1].Get<std::string>("");
	}

	return "";
}

void VisualToolBase::SetSelectedOverride(std::string const& tag, std::string const& value) {
	auto core = c->GetCore();
	for (auto line : core.selectionController->GetSelectedSet())
		SetOverride(line, tag, value);
}

void VisualToolBase::SetOverride(AssDialogue* line, std::string const& tag, std::string const& value) {
	if (!line) return;

	std::string removeTag;
	if (tag == "\\1c") removeTag = "\\c";
	else if (tag == "\\frz") removeTag = "\\fr";
	else if (tag == "\\pos") removeTag = "\\move";
	else if (tag == "\\move") removeTag = "\\pos";
	else if (tag == "\\clip") removeTag = "\\iclip";
	else if (tag == "\\iclip") removeTag = "\\clip";

	// Get block at start
	auto blocks = line->ParseTags();
	AssDialogueBlock *block = blocks.front().get();

	if (block->GetType() == AssBlockType::OVERRIDE) {
		auto ovr = static_cast<AssDialogueBlockOverride*>(block);
		// Remove old of same
		for (size_t i = 0; i < ovr->Tags.size(); i++) {
			std::string const& name = ovr->Tags[i].Name;
			if (tag == name || removeTag == name) {
				ovr->Tags.erase(ovr->Tags.begin() + i);
				i--;
			}
		}
		ovr->AddTag(tag + value);

		line->UpdateText(blocks);
	}
	else
		line->Text = "{" + tag + value + "}" + line->Text.get();
}

// If only export worked
template class VisualTool<VisualDraggableFeature>;
template class VisualTool<ClipCorner>;
template class VisualTool<VisualToolDragDraggableFeature>;
template class VisualTool<VisualToolVectorClipDraggableFeature>;
