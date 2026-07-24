// Pure decision helpers for App/Auto/Reload External Changes.
// Kept header-only so unit tests can exercise arming / post-prompt policy
// without constructing SubsController or a GUI shell.

#pragma once

namespace reload_external_changes {

/// External-change snapshots and overwrite prompts are GUI-shell only.
/// Headless/automation must not hash open files or block Save() on interaction.
inline bool ShouldTrackExternalFileSnapshots(bool is_gui_runtime_shell) noexcept {
	return is_gui_runtime_shell;
}

/// How to arm or disarm live directory watching for one option notification.
struct WatchArmPlan {
	/// Create a WatchedFile if one does not exist.
	bool create_watcher = false;
	/// Bind the current open subtitle path as the watch target.
	bool bind_target = false;
	/// Record a disk baseline only when none exists yet. Never rebaseline solely
	/// because the option was re-notified while already enabled (Preferences
	/// Apply/Reset can SetValue with an unchanged bool).
	bool record_baseline_if_missing = false;
	/// Stop live watching by clearing the target only. Do not destroy the
	/// WatchedFile here: option changes can be nested inside a timer/modal
	/// callback that still owns frames on that object.
	bool disarm_watcher = false;
	/// Clear coalesced external-change pending flags.
	bool clear_pending = false;
};

inline WatchArmPlan PlanWatchArm(
	bool option_enabled,
	bool has_watcher,
	bool has_open_file,
	bool has_baseline) noexcept {
	if (!option_enabled) {
		WatchArmPlan plan;
		plan.disarm_watcher = has_watcher;
		plan.clear_pending = true;
		return plan;
	}

	WatchArmPlan plan;
	plan.create_watcher = !has_watcher;
	plan.bind_target = has_open_file;
	plan.record_baseline_if_missing = has_open_file && !has_baseline;
	return plan;
}

/// What to do after the external-reload modal returns.
///
/// An affirmative answer always reloads: the dialog already asked the user, so
/// their Yes is not gated on whether live detection is still enabled. The option
/// only controls whether we watch/prompt in the future, not whether we honor
/// a choice already made on a shown dialog.
enum class AfterPromptAction {
	/// User agreed: reload from disk (regardless of current detection option).
	Reload,
	/// Remember this disk state as "already prompted" and stop.
	StampPromptedAndStop,
	/// Remember this disk state and loop again (more FS events arrived).
	StampPromptedAndContinue,
};

inline AfterPromptAction PlanAfterPrompt(
	bool user_wants_reload,
	bool option_still_enabled,
	bool has_pending_changes) noexcept {
	if (user_wants_reload)
		return AfterPromptAction::Reload;

	// User declined. If detection was turned off mid-prompt, stop watching
	// loops; still stamp so we do not re-ask for the same disk state while
	// the file is still open with detection re-enabled later only via FS events.
	if (!option_still_enabled)
		return AfterPromptAction::StampPromptedAndStop;

	if (has_pending_changes)
		return AfterPromptAction::StampPromptedAndContinue;

	return AfterPromptAction::StampPromptedAndStop;
}

} // namespace reload_external_changes
