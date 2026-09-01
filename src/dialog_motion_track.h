#pragma once

// Owns the MotionTrackSession; drives Analyze through DialogProgress with a
// Project raw-video lease; applies trajectories through the pure planner.

#include "motion_track/apply_plan.h"
#include "motion_track/session.h"
#include "motion_track/similarity_backend.h"
#include "motion_track/types.h"

#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/spinctrl.h>
#include <wx/combobox.h>
#include <wx/textctrl.h>
#include <wx/button.h>

#include <libaegisub/signal.h>

#include <memory>
#include <typeinfo>
#include <vector>

class AsyncVideoProvider;
class AssDialogue;
namespace agi { struct Context; }

class DialogMotionTrack final : public wxDialog {
	agi::Context* context;
	agi::signal::Connection video_open;
	std::unique_ptr<aegisub::motion_track::MotionTrackSession> session;
	aegisub::motion_track::TranslationTrackerBackend translation_backend;
	aegisub::motion_track::SimilarityTrackerBackend similarity_backend;
	RawVideoIdentity last_identity{};
	int seed_time_ms = 0;
	aegisub::motion_track::TrackDirection last_direction =
	    aegisub::motion_track::TrackDirection::Bidirectional;
	aegisub::motion_track::TrackModel last_model =
	    aegisub::motion_track::TrackModel::Translation;
	int last_target_start_ms = -1;
	int last_target_end_ms = -1;

	/// Ghost positions of the last built apply plan, for the overlay tool:
	/// one polyline per target line, knots in storage pixels. Built by the
	/// "Plan preview" checkbox; cleared on Analyze/Apply.
	struct PlanPreviewPoint {
		float x = 0.f;
		float y = 0.f;
	};
	struct PlanPreviewData {
		std::vector<std::vector<PlanPreviewPoint>> paths;
	};
	std::shared_ptr<const PlanPreviewData> plan_preview_;

	wxSpinCtrl* roi_x = nullptr;
	wxSpinCtrl* roi_y = nullptr;
	wxSpinCtrl* roi_w = nullptr;
	wxSpinCtrl* roi_h = nullptr;
	wxComboBox* direction = nullptr;
	wxComboBox* model = nullptr;
	wxComboBox* apply_mode = nullptr;
	wxSpinCtrl* epsilon = nullptr;
	wxSpinCtrl* decimals = nullptr;
	wxSpinCtrl* smooth = nullptr;
	wxCheckBox* template_refresh = nullptr;
	wxCheckBox* stabilize = nullptr;
	wxCheckBox* growth = nullptr;
	wxCheckBox* apply_fad = nullptr;
	wxCheckBox* preview = nullptr;
	wxTextCtrl* stats = nullptr;
	wxButton* analyze_btn = nullptr;
	wxButton* apply_btn = nullptr;
	wxButton* close_btn = nullptr;

	void OnAnalyze(wxCommandEvent&);
	void OnApply(wxCommandEvent&);
	void OnPreviewToggle(wxCommandEvent&);
	/// Shared by Apply and Plan preview: validates the session/identity,
	/// collects every selected non-comment line (sorted by start time) and
	/// assembles the planner input. Shows its own error box and returns false
	/// when the run cannot proceed.
	bool BuildApplyInput(aegisub::motion_track::ApplyPlanInput& input,
	                     std::vector<AssDialogue*>& targets);
	/// Repaint the video display so overlay changes become visible at once.
	void RefreshVideoDisplay();
	bool BuildSessionFromUi();
	void RefreshReadonlyStats();

	/// Analyze is always available; Apply only once the published trajectory
	/// holds at least one Ok sample. Called on every path that leaves
	/// OnAnalyze so the pair never gets stuck disabled.
	void RefreshButtons();

	/// Enable/disable the apply options that only make sense for some
	/// model/mode combinations (growth needs Similarity + Exact); called on
	/// construction and whenever those combos change.
	void UpdateApplyOptionAvailability();

	/// Reuse the existing session (Continue semantics: same video identity,
	/// target line and direction) or build a fresh one that discards the
	/// trajectory. Updates the cached continue keys either way.
	std::unique_ptr<aegisub::motion_track::MotionTrackSession>
	ContinueOrRebuild(aegisub::motion_track::SessionDomains const& domains,
	                  aegisub::motion_track::RoiRect roi,
	                  aegisub::motion_track::TrackDirection dir,
	                  aegisub::motion_track::TrackModel model,
	                  AssDialogue* target);

	/// Swap the motion-track overlay tool back to the cross tool. No-op when
	/// the video display is already gone or some other tool has taken over
	/// since, so it is safe to call from the destructor.
	void ResetOverlayTool();

	void OnVideoOpen(AsyncVideoProvider *new_provider);

	public:
	/// Re-attach the ROI overlay tool to the current video display unless it
	/// is already active. The command entry point lands here on every
	/// invocation: switching to another visual tool destroyed the previous
	/// overlay instance, so without this re-attach the ROI box would stay
	/// gone until the dialog is closed and reopened.
	void EnsureOverlayOnCurrentDisplay();

	/// Accepted ROI geometry. The spin controls are built from these and the
	/// overlay tool clamps mouse drags to them, so a box drawn on the video and
	/// one typed in can never disagree about what is accepted -- otherwise
	/// SetOverlayRoi's silent spin clamping would store a different rectangle
	/// than the one the user dragged.
	static constexpr int kMinRoiSide = 8;
	static constexpr int kMaxRoiSide = 512;
	static constexpr int kMaxRoiOrigin = 4096;

	DialogMotionTrack(agi::Context* context);
	~DialogMotionTrack();

	// Overlay-tool interface (visual_tool_motion_track reads these).
	std::shared_ptr<const aegisub::motion_track::MotionTrackSnapshot>
	CaptureForOverlay() const;
	std::shared_ptr<const PlanPreviewData> CapturePlanPreview() const {
		return plan_preview_;
	}
	aegisub::motion_track::RoiRect OverlayRoi() const;
	void SetOverlayRoi(aegisub::motion_track::RoiRect roi);
};
