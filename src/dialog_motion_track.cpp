// Interaction contract:
//  - Analyze runs on a DialogProgress task thread with a Project raw-video
//    lease held for the whole run; user cancellation surfaces as
//    agi::UserCancelException thrown by DialogProgress::Run on this thread
//    AFTER the task has committed its prefix — caught here.
//  - Apply runs the pure planner and executes the returned parts in a single
//    AssFile::Commit.
//  - Close destroys the session along with the window.

#include "dialog_motion_track.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info_service.h"
#include "async_video_provider.h"
#include "compat.h"
#include "dialog_manager.h"
#include "dialog_progress.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "motion_track/raw_batch_motion_frame_reader.h"
#include "motion_track/similarity_backend.h"
#include "options.h"
#include <libaegisub/make_unique.h>
#include "project.h"
#include "visual_tool_cross.h"
#include "visual_tool_motion_track.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/vfr.h>

#include <wx/button.h>
#include <wx/combobox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

using namespace aegisub::motion_track;

namespace {

/// Reader used when hooks.run_batch performs all fetching; never queried.
class NullMotionFrameReader final : public MotionFrameReader {
	FrameReadResult FetchGray(int, RoiRect, GrayPatch&) override {
		return {FrameReadStatus::Error, "fetching requires run_batch"};
	}
};

/// Maps a batch-scoped FrameReadResult onto the RawVideoBatchStatus surface
/// RunRawVideoBatch reports. Non-Ok results never map to Completed so the
/// session's stop-reason mapping stays faithful.
RawVideoBatchStatus ToBatchStatus(FrameReadResult const& read) {
	switch (read.status) {
		case FrameReadStatus::Ok:
			return RawVideoBatchStatus::Completed;
		case FrameReadStatus::ProviderChanged:
			return RawVideoBatchStatus::ProviderChanged;
		case FrameReadStatus::FrameUnavailable:
			return RawVideoBatchStatus::FrameUnavailable;
		default:
			return RawVideoBatchStatus::DecodeError;
	}
}

std::unique_ptr<AssDialogue> MakePartDialogue(AssDialogue const& source,
                                              PlannedLinePart const& part) {
	auto line = std::make_unique<AssDialogue>();
	line->Layer = source.Layer;
	line->Margin = source.Margin;
	line->Style = source.Style;
	line->Actor = source.Actor;
	line->Effect = source.Effect;
	line->Comment = source.Comment;
	line->Start = part.start_ms;
	line->End = part.end_ms;
	line->Text = part.text;
	return line;
}

/// Deterministic target-line choice shared by Analyze and Apply: the active
/// line wins; otherwise the earliest selected line by start time (the
/// selection set is unordered, so iterating it directly would be random).
/// Returns nullptr when there is no line at all.
AssDialogue* PickTargetLine(agi::Context* c) {
	auto const& core = c->GetCore();
	if (auto* active = core.selectionController->GetActiveLine())
		return active;
	AssDialogue* best = nullptr;
	for (auto* l : core.selectionController->GetSelectedSet()) {
		if (!best || std::make_pair(int(l->Start), int(l->End))
		               < std::make_pair(int(best->Start), int(best->End)))
			best = l;
	}
	return best;
}

} // namespace

DialogMotionTrack::DialogMotionTrack(agi::Context *c)
	: wxDialog(c->GetUI().parent, -1, _("Motion Track")), context(c), video_open(c->GetCore().project->AddVideoProviderListener(&DialogMotionTrack::OnVideoOpen,
																																this)) {
	auto* root = new wxBoxSizer(wxVERTICAL);

	auto* roi_grid = new wxFlexGridSizer(4, 4, 5, 5);
	auto add_spin = [&](wxString const& label, int value, int minv, int maxv)
	    -> wxSpinCtrl* {
		roi_grid->Add(new wxStaticText(this, -1, label),
		              0, wxALIGN_CENTRE_VERTICAL);
		auto* spin = new wxSpinCtrl(this);
		spin->SetRange(minv, maxv);
		spin->SetValue(value);
		roi_grid->Add(spin, 1, wxEXPAND);
		return spin;
	};

	roi_x = add_spin(_("ROI X"), 40, 0, kMaxRoiOrigin);
	roi_y = add_spin(_("ROI Y"), 30, 0, kMaxRoiOrigin);
	roi_w = add_spin(_("ROI W"), 24, kMinRoiSide, kMaxRoiSide);
	roi_h = add_spin(_("ROI H"), 16, kMinRoiSide, kMaxRoiSide);
	root->Add(roi_grid, 0, wxALL | wxEXPAND, 5);

	// Options are sectioned: everything that steers the tracker lives in
	// "Tracking", everything that shapes the written output in "Apply", so
	// per-mode availability (growth needs Similarity + Exact) reads as part
	// of the layout instead of a surprise.
	auto* track_box = new wxStaticBox(this, -1, _("Tracking"));
	auto* track_grid = new wxFlexGridSizer(2, 2, 5, 5);
	track_grid->Add(new wxStaticText(track_box, -1, _("Direction")),
	                0, wxALIGN_CENTRE_VERTICAL);
	direction = new wxComboBox(track_box, -1, _("Both directions"));
	direction->Append(_("Forward"));
	direction->Append(_("Backward"));
	direction->Append(_("Both directions"));
	direction->SetSelection(std::clamp(
	    int(OPT_GET("Tool/Motion Track/Direction")->GetInt()), 0, 2));
	track_grid->Add(direction, 1, wxEXPAND);

	track_grid->Add(new wxStaticText(track_box, -1, _("Model")),
	                0, wxALIGN_CENTRE_VERTICAL);
	model = new wxComboBox(track_box, -1, _("Translation"));
	model->Append(_("Translation"));
	model->Append(_("Similarity (rotation+scale)"));
	model->SetSelection(std::clamp(
	    int(OPT_GET("Tool/Motion Track/Model")->GetInt()), 0, 1));
	model->SetToolTip(_("Similarity tracks rotation and uniform scale too; "
	                    "Apply needs Exact mode"));
	track_grid->Add(model, 1, wxEXPAND);

	track_grid->Add(new wxStaticText(track_box, -1, _("Template refresh")),
	                0, wxALIGN_CENTRE_VERTICAL);
	template_refresh = new wxCheckBox(track_box, -1, wxString());
	template_refresh->SetValue(
	    OPT_GET("Tool/Motion Track/Template Refresh")->GetBool());
	track_grid->Add(template_refresh, 1, wxEXPAND);

	auto* track_sizer = new wxStaticBoxSizer(track_box, wxVERTICAL);
	track_sizer->Add(track_grid, 1, wxEXPAND | wxALL, 5);
	root->Add(track_sizer, 0, wxALL | wxEXPAND, 5);

	auto* apply_box = new wxStaticBox(this, -1, _("Apply"));
	auto* apply_grid = new wxFlexGridSizer(2, 2, 5, 5);
	apply_grid->Add(new wxStaticText(apply_box, -1, _("Apply mode")),
	                0, wxALIGN_CENTRE_VERTICAL);
	apply_mode = new wxComboBox(apply_box, -1, _("Compact"));
	apply_mode->Append(_("Compact"));
	apply_mode->Append(_("Exact"));
	apply_mode->SetSelection(std::clamp(
	    int(OPT_GET("Tool/Motion Track/Apply Mode")->GetInt()), 0, 1));
	apply_grid->Add(apply_mode, 1, wxEXPAND);

	apply_grid->Add(new wxStaticText(apply_box, -1, _("Compact error (1/100 video px)")),
	                0, wxALIGN_CENTRE_VERTICAL);
	epsilon = new wxSpinCtrl(apply_box);
	epsilon->SetRange(1, 1000);
	// The option is in storage (video) pixels (0.75); the control is in
	// 1/100 px, so the threshold means the same on-screen error at every
	// script resolution.
	epsilon->SetValue(std::clamp(
	    int(std::lround(
	        OPT_GET("Tool/Motion Track/Compact Error")->GetDouble() * 100.0)),
	    1, 1000));
	apply_grid->Add(epsilon, 1, wxEXPAND);

	apply_grid->Add(new wxStaticText(apply_box, -1, _("Position decimals")),
	                0, wxALIGN_CENTRE_VERTICAL);
	decimals = new wxSpinCtrl(apply_box);
	decimals->SetRange(0, 6);
	decimals->SetValue(std::clamp(
	    int(OPT_GET("Tool/Motion Track/Position Decimals")->GetInt()), 0, 6));
	apply_grid->Add(decimals, 1, wxEXPAND);

	apply_grid->Add(new wxStaticText(apply_box, -1, _("Smooth (frames)")),
	                0, wxALIGN_CENTRE_VERTICAL);
	smooth = new wxSpinCtrl(apply_box);
	// Local-linear smoothing half-window: removes per-frame tracker jitter
	// before any simplification, without lagging real constant-velocity
	// motion. 0 disables.
	smooth->SetRange(0, 15);
	smooth->SetValue(std::clamp(
	    int(OPT_GET("Tool/Motion Track/Smooth Frames")->GetInt()), 0, 15));
	apply_grid->Add(smooth, 1, wxEXPAND);

	apply_grid->Add(new wxStaticText(apply_box, -1, _("Stabilize trajectory")),
	                0, wxALIGN_CENTRE_VERTICAL);
	stabilize = new wxCheckBox(apply_box, -1, wxString());
	stabilize->SetToolTip(_(
	    "Remove tracker jitter and flatten noise-only holds before writing "
	    "positions; exact for linear motion"));
	stabilize->SetValue(
	    OPT_GET("Tool/Motion Track/Stabilize")->GetBool());
	apply_grid->Add(stabilize, 1, wxEXPAND);

	apply_grid->Add(new wxStaticText(apply_box, -1, _("Scale border/shadow/blur")),
	                0, wxALIGN_CENTRE_VERTICAL);
	growth = new wxCheckBox(apply_box, -1, wxString());
	growth->SetToolTip(_(
	   	"Similarity Exact applies only: scale \\bord, \\shad and \\blur with "
	   	"the tracked zoom so outline, shadow and blur follow the object"));
	growth->SetValue(
	    OPT_GET("Tool/Motion Track/Scale Border Shadow Blur")->GetBool());
	apply_grid->Add(growth, 1, wxEXPAND);

	apply_grid->Add(new wxStaticText(apply_box, -1, _("Write \\fad from fade")),
	                0, wxALIGN_CENTRE_VERTICAL);
	apply_fad = new wxCheckBox(apply_box, -1, wxString());
	apply_fad->SetToolTip(_(
		"When Analyze detected a fade, write its timing as \\fad on applied "
		"lines; without a detected fade this does nothing"));
	apply_fad->SetValue(
	    OPT_GET("Tool/Motion Track/Apply Fad")->GetBool());
	apply_grid->Add(apply_fad, 1, wxEXPAND);

	apply_grid->Add(new wxStaticText(apply_box, -1, _("Plan preview")),
	                0, wxALIGN_CENTRE_VERTICAL);
	preview = new wxCheckBox(apply_box, -1, wxString());
	preview->SetToolTip(_("Show the positions Apply would write as a ghost path on the video"));
	apply_grid->Add(preview, 1, wxEXPAND);

	auto* apply_sizer = new wxStaticBoxSizer(apply_box, wxVERTICAL);
	apply_sizer->Add(apply_grid, 1, wxEXPAND | wxALL, 5);
	root->Add(apply_sizer, 0, wxALL | wxEXPAND, 5);

	stats = new wxTextCtrl(this, -1, wxEmptyString, wxDefaultPosition,
	                       wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
	stats->SetMinSize(wxSize(380, 110));
	root->Add(stats, 1, wxALL | wxEXPAND, 5);

	auto* buttons = new wxBoxSizer(wxHORIZONTAL);
	analyze_btn = new wxButton(this, -1, _("Analyze"));
	apply_btn = new wxButton(this, -1, _("Apply"));
	close_btn = new wxButton(this, wxID_CANCEL, _("Close"));
	buttons->Add(analyze_btn, 0, wxRIGHT, 5);
	buttons->Add(apply_btn, 0, wxRIGHT, 5);
	buttons->AddStretchSpacer();
	buttons->Add(close_btn, 0);
	root->Add(buttons, 0, wxALL | wxEXPAND, 5);

	analyze_btn->Bind(wxEVT_BUTTON, &DialogMotionTrack::OnAnalyze, this);
	apply_btn->Bind(wxEVT_BUTTON, &DialogMotionTrack::OnApply, this);
	preview->Bind(wxEVT_CHECKBOX, &DialogMotionTrack::OnPreviewToggle, this);
	model->Bind(wxEVT_COMBOBOX,
	            [this](wxCommandEvent&) { UpdateApplyOptionAvailability(); });
	apply_mode->Bind(
		wxEVT_COMBOBOX,
		[this](wxCommandEvent&) { UpdateApplyOptionAvailability(); });
	UpdateApplyOptionAvailability();
	RefreshButtons(); // no trajectory yet, so Apply starts disabled
	// No handler on close_btn: DialogManager::ShowOnce binds wxEVT_BUTTON for
	// wxID_CANCEL on the dialog itself and that is what unregisters us from
	// created_dialogs. Handling the click here without Skip() would leave a
	// dangling entry behind, and a later ShowOnce would resurrect a destroyed
	// window.

	SetSizerAndFit(root);
	CentreOnParent();
	SetIcon(GETICON(motion_track_button_16));

	// Attach the overlay visual tool to the current display so users can
	// aim/drag the ROI box directly on the video.
	EnsureOverlayOnCurrentDisplay();
}

void DialogMotionTrack::OnVideoOpen(AsyncVideoProvider *) {
	// A different video invalidates the trajectory; Analyze would rebuild
	// anyway, but dropping it here keeps the stats/Apply state honest.
	if (session) {
		auto *provider = context->GetCore().project->VideoProvider();
		RawVideoIdentity const identity =
			provider ? provider->GetRawVideoIdentity() : RawVideoIdentity{};
		if (!identity.Matches(last_identity)) {
			session.reset();
			// A stale preview lives in the old video's coordinates; it must
			// not survive the trajectory it was computed from.
			plan_preview_.reset();
			preview->SetValue(false);
			RefreshButtons();
			RefreshReadonlyStats();
			RefreshVideoDisplay();
		}
	}
	// Opening a video recreates the display (or resets its tool); bring the
	// ROI overlay back so the session stays editable without reopening.
	EnsureOverlayOnCurrentDisplay();
}

void DialogMotionTrack::EnsureOverlayOnCurrentDisplay() {
	auto *display = context->GetUI().videoDisplay;
	if (!display || display->ToolIsType(typeid(VisualToolMotionTrack)))
		return;
	display->SetTool(agi::make_unique<VisualToolMotionTrack>(display, context));
}

DialogMotionTrack::~DialogMotionTrack() {
	OPT_SET("Tool/Motion Track/Direction")
	    ->SetInt(std::max(0, direction->GetSelection()));
	OPT_SET("Tool/Motion Track/Model")
	    ->SetInt(std::max(0, model->GetSelection()));
	OPT_SET("Tool/Motion Track/Apply Mode")
	    ->SetInt(std::max(0, apply_mode->GetSelection()));
	OPT_SET("Tool/Motion Track/Compact Error")
	    ->SetDouble(epsilon->GetValue() / 100.0);
	OPT_SET("Tool/Motion Track/Position Decimals")
	    ->SetInt(decimals->GetValue());
	OPT_SET("Tool/Motion Track/Smooth Frames")
	    ->SetInt(smooth->GetValue());
	OPT_SET("Tool/Motion Track/Template Refresh")
	    ->SetBool(template_refresh->GetValue());
	OPT_SET("Tool/Motion Track/Stabilize")
	    ->SetBool(stabilize->GetValue());
	OPT_SET("Tool/Motion Track/Scale Border Shadow Blur")
	    ->SetBool(growth->GetValue());
	OPT_SET("Tool/Motion Track/Apply Fad")
	    ->SetBool(apply_fad->GetValue());
	ResetOverlayTool();
}

void DialogMotionTrack::ResetOverlayTool() {
	auto* display = context->GetUI().videoDisplay;
	if (!display || !display->ToolIsType(typeid(VisualToolMotionTrack)))
		return;
	display->SetTool(agi::make_unique<VisualToolCross>(display, context));
}

std::unique_ptr<MotionTrackSession> DialogMotionTrack::ContinueOrRebuild(
	SessionDomains const& domains, RoiRect roi, TrackDirection dir,
	TrackModel track_model, AssDialogue* target) {
	auto* provider = context->GetCore().project->VideoProvider();
	RawVideoIdentity const identity =
	    provider ? provider->GetRawVideoIdentity() : RawVideoIdentity{};

	bool const can_continue = session && identity.Matches(last_identity)
	                       && dir == last_direction
	                       && track_model == last_model
	                       && int(target->Start) == last_target_start_ms
	                       && int(target->End) == last_target_end_ms;

	last_identity = identity;
	last_direction = dir;
	last_model = track_model;
	last_target_start_ms = int(target->Start);
	last_target_end_ms = int(target->End);

	if (can_continue) {
		// Continue: origin seed and committed samples survive; SetBackendSeed
		// moves only the backend seed onto the new ROI/frame.
		return std::move(session);
	}
	return std::make_unique<MotionTrackSession>(domains, roi, dir,
	                                            track_model);
}

bool DialogMotionTrack::BuildSessionFromUi() {
	auto core = context->GetCore();
	auto* provider = core.project->VideoProvider();
	if (!provider) {
		wxMessageBox(_("No video is open."), _("Motion Track"),
		             wxOK | wxICON_ERROR, this);
		return false;
	}

	RoiRect roi{roi_x->GetValue(), roi_y->GetValue(),
	            roi_w->GetValue(), roi_h->GetValue()};

	auto const& fps = core.project->Timecodes();
	int const frame_count =
	    std::max(0, provider ? provider->GetFrameCount() : 0);

	AssDialogue* target = PickTargetLine(context);
	if (!target || target->Comment) {
		wxMessageBox(_("No subtitle line selected."), _("Motion Track"),
		             wxOK | wxICON_ERROR, this);
		return false;
	}

	int const first = std::clamp(
	    fps.FrameAtTime(int(target->Start), agi::vfr::Time::START),
	    0, frame_count - 1);
	int const last = std::clamp(
	    fps.FrameAtTime(int(target->End), agi::vfr::Time::END),
	    0, frame_count - 1);
	if (last < first) {
		wxMessageBox(_("Selected line has an empty time range."),
		             _("Motion Track"), wxOK | wxICON_ERROR, this);
		return false;
	}

	SessionDomains domains;
	// decode_interval is the continuous hull of the selected frame
	// intervals — for this single-line slice that is the line's own range,
	// not the whole video (whole-video spans trip the 1500/10000-frame
	// range caps regardless of how little actually gets tracked).
	domains.decode_interval = FrameInterval{first, last};
	domains.direction_domain = FrameInterval{first, last};
	domains.video_frame_count = frame_count;
	// Storage dims travel into the published snapshot; the overlay needs them
	// to map storage coordinates onto the display, so leaving them at 0
	// silently disables all overlay drawing.
	domains.storage_width = provider->GetWidth();
	domains.storage_height = provider->GetHeight();
		// Hidden advanced knob: 0 keeps the ROI-derived default radius.
		domains.search_radius_override =
		    std::max(0, int(OPT_GET("Tool/Motion Track/Search Radius")->GetInt()));

	TrackDirection dir = TrackDirection::Bidirectional;
	switch (direction->GetSelection()) {
		case 0: dir = TrackDirection::Forward; break;
		case 1: dir = TrackDirection::Backward; break;
		default: dir = TrackDirection::Bidirectional; break;
	}
	TrackModel track_model = model->GetSelection() == 1
	    ? TrackModel::Similarity
	    : TrackModel::Translation;

	session = ContinueOrRebuild(domains, roi, dir, track_model, target);

	int const seed_frame =
	    std::clamp(core.videoController->GetPresentedFrameN(), first, last);
	double const cx = roi.x + (roi.w - 1) / 2.0;
	double const cy = roi.y + (roi.h - 1) / 2.0;
	session->SetBackendSeed(seed_frame, roi, cx, cy);
	// The origin resolver interpolates an existing \move at seed_time_ms, so
	// this has to be the seed frame's own time — using the line start would
	// read the wrong \move position whenever the seed is not the first frame.
	seed_time_ms = fps.TimeAtFrame(seed_frame, agi::vfr::Time::START);

	return true;
}

void DialogMotionTrack::OnAnalyze(wxCommandEvent&) {
	// A stale preview must not survive a new trajectory.
	plan_preview_.reset();
	preview->SetValue(false);
	if (!BuildSessionFromUi()) return;

	auto core = context->GetCore();
	core.videoController->Stop();

	// Runtime knobs for the tracking backend, re-read per run so toggling the
	// checkbox takes effect on the next Analyze without reopening the dialog.
	TrackerBackend* backend = model->GetSelection() == 1
	    ? static_cast<TrackerBackend*>(&similarity_backend)
	    : static_cast<TrackerBackend*>(&translation_backend);
	if (model->GetSelection() != 1) {
		aegisub::motion_track::TranslationTrackerConfig backend_config;
		backend_config.template_refresh = template_refresh->GetValue();
		translation_backend.SetConfig(backend_config);
	}

	analyze_btn->Disable();
	apply_btn->Disable();

	// decode spans over 1500 frames need explicit confirmation before
	// Analyze starts (>10000 is already rejected inside the session).
	if (session->CheckRange() == RangeCheck::NeedsConfirmation) {
		auto answer = wxMessageBox(
		    _("The tracking range exceeds 1500 frames and may take a while.\n"
		      "Continue?"),
		    _("Motion Track"), wxYES_NO | wxICON_QUESTION, this);
		if (answer != wxYES) {
			RefreshButtons();
			return;
		}
	}

	auto lease_handle = std::make_shared<VideoProviderLeaseState::Handle>(
	    core.project->AcquireVideoProviderLease());
	if (!*lease_handle) {
		wxMessageBox(_("Video is being replaced; try again."), _("Motion Track"),
		             wxOK | wxICON_WARNING, this);
		RefreshButtons();
		return;
	}

	auto const identity = core.project->VideoProvider()->GetRawVideoIdentity();
	last_identity = identity;

	NullMotionFrameReader null_reader;

	DialogProgress progress(this, _("Motion Track"), _("Tracking..."), true);
	try {
		progress.Run([&, identity](agi::ProgressSink* sink) {
			AnalyzeRunHooks hooks;
			hooks.user_cancelled = [sink] { return sink->IsCancelled(); };
			hooks.progress = [&](int done, int total) {
				if (total > 0) sink->SetProgress(done, total);
			};
				hooks.run_batch = [&, identity](
				                      std::function<FrameReadResult(
				                          MotionFrameReader&)> const& fn) {
					auto thunk = [&](RawFrameAccess& access) {
						RawBatchMotionFrameReader batched(access);
						return ToBatchStatus(fn(batched));
					};
					return core.project->VideoProvider()
					    ->RunRawVideoBatch(identity, thunk)
					    .status;
				};
				session->RunAnalyze(null_reader, *backend, *lease_handle, hooks);
		});
	} catch (agi::UserCancelException const&) {
		// Expected on cancel; the committed prefix is already published.
	} catch (agi::Exception const& e) {
		wxMessageBox(to_wx(e.GetMessage()), _("Motion Track"),
		             wxOK | wxICON_ERROR, this);
	}

	RefreshButtons();
	RefreshReadonlyStats();
	RefreshVideoDisplay();
}
bool DialogMotionTrack::BuildApplyInput(ApplyPlanInput& input,
                                        std::vector<AssDialogue*>& targets) {
	auto snap = session ? session->Capture() : nullptr;
	if (!snap || snap->samples.empty()) {
		wxMessageBox(_("Nothing to apply — run Analyze first."),
		             _("Motion Track"), wxOK | wxICON_INFORMATION, this);
		return false;
	}
	auto core = context->GetCore();
	auto* provider = core.project->VideoProvider();
	if (!provider || !provider->GetRawVideoIdentity().Matches(last_identity)) {
		wxMessageBox(_("The video changed since Analyze; re-run Analyze first."),
		             _("Motion Track"), wxOK | wxICON_ERROR, this);
		return false;
	}
	// Apply to every selected non-comment line. Re-resolved NOW: pointers
	// captured at Analyze time can dangle if the user edited the grid since.
	for (auto* l : core.selectionController->GetSelectedSet())
		if (!l->Comment) targets.push_back(l);
	if (targets.empty()) {
		wxMessageBox(_("No subtitle line selected."), _("Motion Track"),
		             wxOK | wxICON_ERROR, this);
		return false;
	}
	// Deterministic order matching the grid, independent of set iteration.
	std::sort(targets.begin(), targets.end(),
	          [](AssDialogue const* a, AssDialogue const* b) {
		          return std::make_pair(int(a->Start), int(a->End))
			              < std::make_pair(int(b->Start), int(b->End));
	          });

	input.samples = snap->samples;
	input.model = snap->model;
	input.origin_center_x = snap->origin_center_x;
	input.origin_center_y = snap->origin_center_y;
	input.decode_interval = snap->decode_interval;
	input.direction_domain = snap->direction_domain;
	input.storage_width = last_identity.width;
	input.storage_height = last_identity.height;
	auto& ass = *core.ass;
	// Layout resolution: LayoutResX/Y when the script sets them, PlayRes
	// otherwise — \pos/\move live in the layout space renderers actually use.
	int layout_w = 0, layout_h = 0;
	ass.GetResolution(ScriptResolutionType::LayoutRes, layout_w, layout_h);
	input.script_width = std::max(1, layout_w);
	input.script_height = std::max(1, layout_h);
	input.timecodes = core.project->Timecodes();
	input.video_frame_count = last_identity.frame_count;
	input.seed_time_ms = seed_time_ms;
	input.options.mode =
	    apply_mode->GetSelection() == 1 ? ApplyMode::Exact : ApplyMode::Compact;
	input.options.compact_epsilon = epsilon->GetValue() / 100.0;
	input.options.position_decimals = decimals->GetValue();
	input.options.smooth_frames = smooth->GetValue();
	input.options.stabilization.enable = stabilize->GetValue();
	bool const growth_on = growth->GetValue();
	input.options.scale_border = growth_on;
	input.options.scale_shadow = growth_on;
	input.options.scale_blur = growth_on;
	input.options.apply_fad = apply_fad->GetValue();
	return true;
}

void DialogMotionTrack::OnApply(wxCommandEvent&) {
	ApplyPlanInput input;
	std::vector<AssDialogue*> apply_targets;
	if (!BuildApplyInput(input, apply_targets)) return;
	auto core = context->GetCore();
	auto& ass = *core.ass;

	if (session->LastStopReason() != AnalyzeStopReason::Completed) {
		auto answer = wxMessageBox(
		    _("The trajectory is incomplete (tracking was interrupted).\n"
		      "Applying will fail unless every needed frame is covered.\n\n"
		      "Try to apply anyway?"),
		    _("Motion Track"), wxYES_NO | wxICON_WARNING, this);
		if (answer != wxYES) return;
	}

	auto plan = BuildApplyPlan(ass, apply_targets, input);
	if (plan.status == ApplyPlanStatus::IncompleteCoverage) {
		std::string msg = "Trajectory does not cover:\n";
		for (auto const& l : plan.uncovered)
			for (auto const& r : l.ranges)
				msg += "  frames " + std::to_string(r.first) + ".." +
				       std::to_string(r.last) + "\n";
		wxMessageBox(to_wx(msg), _("Motion Track"), wxOK | wxICON_WARNING, this);
		return;
	}
	if (!plan.has_mutations()) {
		wxMessageBox(to_wx(plan.message), _("Motion Track"),
		             wxOK | wxICON_ERROR, this);
		return;
	}
	if (plan.needs_confirmation()) {
		auto answer = wxMessageBox(
		    to_wx("This will create " + std::to_string(plan.event_count) +
		          " events.\nContinue?"),
		    _("Motion Track"), wxYES_NO | wxICON_QUESTION, this);
		if (answer != wxYES) return;
	}

	// Defer freeing the source lines until after the selection has been moved
	// off them: the selection set and the active line still point at the
	// originals, and dropping them first leaves dangling pointers behind.
	std::vector<std::unique_ptr<AssDialogue>> to_delete;
	Selection new_sel;
	AssDialogue* new_active = nullptr;
	for (auto const& pl : plan.lines) {
		// Always insert before the source line: parts accumulate in ascending
		// time order and the source is erased afterwards. Advancing an anchor
		// (inserting before the previous part) reverses the order.
		for (auto const& part : pl.parts) {
			auto newline = MakePartDialogue(*pl.source, part).release();
			ass.Events.insert(ass.iterator_to(*pl.source), *newline);
			new_sel.insert(newline);
			if (!new_active) new_active = newline;
		}
		ass.Events.erase(ass.iterator_to(*pl.source));
		to_delete.emplace_back(pl.source);
	}

	ass.Commit(from_wx(_("Apply motion track")),
	           AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_DIAG_ADDREM |
	               AssFile::COMMIT_DIAG_TIME);
	if (new_active)
		core.selectionController->SetSelectionAndActive(std::move(new_sel),
		                                               new_active);
	plan_preview_.reset();
	preview->SetValue(false);
	RefreshReadonlyStats();
	RefreshVideoDisplay();
}

void DialogMotionTrack::OnPreviewToggle(wxCommandEvent&) {
	plan_preview_.reset();
	if (preview->GetValue()) {
		ApplyPlanInput input;
		std::vector<AssDialogue*> targets;
		if (BuildApplyInput(input, targets)) {
			auto plan = BuildApplyPlan(*context->GetCore().ass, targets, input);
			if (plan.has_mutations()) {
				auto data = std::make_shared<PlanPreviewData>();
				double const scale_x =
				    double(last_identity.width) / std::max(1, input.script_width);
				double const scale_y =
				    double(last_identity.height) / std::max(1, input.script_height);
				for (auto const& pl : plan.lines) {
					std::vector<PlanPreviewPoint> path;
					for (auto const& part : pl.parts) {
						if (!part.covered) continue;
						path.push_back(PlanPreviewPoint{
						    float(part.x0 * scale_x), float(part.y0 * scale_y)});
					}
					// Trailing knot of the last covered part closes the path.
					for (auto it = pl.parts.rbegin(); it != pl.parts.rend(); ++it)
						if (it->covered) {
							path.push_back(PlanPreviewPoint{
							    float(it->x1 * scale_x), float(it->y1 * scale_y)});
							break;
						}
					if (path.size() >= 2)
						data->paths.push_back(std::move(path));
				}
				plan_preview_ = std::move(data);
			} else {
				// Surface the reason in the stats box instead of popping a
				// modal from a checkbox toggle.
				preview->SetValue(false);
				stats->SetValue(to_wx(plan.message.empty()
				                          ? std::string("preview unavailable")
				                          : plan.message));
			}
		} else {
			preview->SetValue(false);
		}
	}
	RefreshVideoDisplay();
}

void DialogMotionTrack::RefreshVideoDisplay() {
	if (auto* display = context->GetUI().videoDisplay)
		display->Render();
}

static std::string StopReasonText(AnalyzeStopReason reason) {
	switch (reason) {
		case AnalyzeStopReason::None: return "none";
		case AnalyzeStopReason::Completed: return "completed";
		case AnalyzeStopReason::UserCanceled: return "canceled by user";
		case AnalyzeStopReason::ConsecutiveFailures:
			return "stopped after repeated failures";
		case AnalyzeStopReason::ProviderChanged: return "video changed";
		case AnalyzeStopReason::FrameUnavailable: return "frame unavailable";
		case AnalyzeStopReason::DecodeError: return "decode error";
		case AnalyzeStopReason::Error: return "error";
	}
	return "unknown";
}

void DialogMotionTrack::RefreshButtons() {
	analyze_btn->Enable();
	auto snap = session ? session->Capture() : nullptr;
	// A trajectory of nothing but Failed/Missing samples cannot produce a
	// single \pos, and OnApply would only pop an error box.
	apply_btn->Enable(snap && snap->success_count > 0);
}

void DialogMotionTrack::UpdateApplyOptionAvailability() {
	// Growth compensation only exists in similarity Exact applies; leaving
	// the checkbox enabled there would silently do nothing.
	bool const similarity = model->GetSelection() == 1;
	bool const exact = apply_mode->GetSelection() == 1;
	growth->Enable(similarity && exact);
}

void DialogMotionTrack::RefreshReadonlyStats() {
	auto snap = session ? session->Capture() : nullptr;
	if (!snap) {
		stats->SetValue(wxEmptyString);
		return;
	}
	std::string text = agi::format(
	    "ok/failed: %d/%d\nseed frame: %d\nstop reason: %s",
	    snap->success_count, snap->failure_count, snap->backend_seed_frame,
	    StopReasonText(snap->stop_reason));
	if (!snap->diagnostic_message.empty())
		text += "\ndiagnostic: " + snap->diagnostic_message;
	if (snap->stop_reason == AnalyzeStopReason::UserCanceled
	    || snap->stop_reason == AnalyzeStopReason::ConsecutiveFailures)
		text += "\nhint: drag the ROI back onto the object and press Analyze "
		        "again — tracking continues from there (origin kept)";
	stats->SetValue(to_wx(text));
}

std::shared_ptr<const MotionTrackSnapshot>
DialogMotionTrack::CaptureForOverlay() const {
	return session ? session->Capture() : nullptr;
}

RoiRect DialogMotionTrack::OverlayRoi() const {
	return RoiRect{roi_x->GetValue(), roi_y->GetValue(),
	                roi_w->GetValue(), roi_h->GetValue()};
}

void DialogMotionTrack::SetOverlayRoi(RoiRect roi) {
	roi_x->SetValue(roi.x);
	roi_y->SetValue(roi.y);
	roi_w->SetValue(roi.w);
	roi_h->SetValue(roi.h);
}

void ShowMotionTrackDialog(agi::Context* c) {
	// Show delegates to the single-instance path: a second invocation focuses
	// the existing window instead of stacking a duplicate.
	c->GetUI().dialog->Show<DialogMotionTrack>(c);
	// The overlay tool is destroyed whenever another visual tool takes over;
	// re-attach it so one click on the command restores ROI interaction
	// without closing and reopening the dialog.
	if (auto *dialog = c->GetUI().dialog->Get<DialogMotionTrack>())
		dialog->EnsureOverlayOnCurrentDisplay();
}
