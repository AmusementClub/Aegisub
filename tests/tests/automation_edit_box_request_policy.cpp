#include <main.h>

#include "../../src/automation/automation_edit_box_request_policy.h"
#include "../../src/subtitle_edit_box_focus.h"

namespace {

std::optional<Automation4::AutomationRuntimeStateSnapshot> MakeRuntimeState(Automation4::AutomationInvocation invocation) {
	Automation4::AutomationRuntimeStateSnapshot snapshot;
	snapshot.invocation = std::move(invocation);
	return snapshot;
}

TEST(AutomationEditBoxRequestPolicy, rejects_requests_without_runtime_state) {
	EXPECT_FALSE(Automation4::CanQueueSubtitleEditBoxRequest(std::nullopt, true));
}

TEST(AutomationEditBoxRequestPolicy, rejects_requests_when_edit_box_cannot_focus) {
	auto runtime_state = MakeRuntimeState(Automation4::MakeMacroRunInvocation("Edit box"));

	EXPECT_FALSE(Automation4::CanQueueSubtitleEditBoxRequest(runtime_state, false));
}

TEST(AutomationEditBoxRequestPolicy, only_allows_macro_run_invocations) {
	EXPECT_TRUE(Automation4::CanQueueSubtitleEditBoxRequest(
		MakeRuntimeState(Automation4::MakeMacroRunInvocation("Edit box")), true));
	EXPECT_FALSE(Automation4::CanQueueSubtitleEditBoxRequest(
		MakeRuntimeState(Automation4::MakeMacroIsActiveInvocation("Edit box")), true));
	EXPECT_FALSE(Automation4::CanQueueSubtitleEditBoxRequest(
		MakeRuntimeState(Automation4::MakeMacroValidateInvocation("Edit box")), true));
	EXPECT_FALSE(Automation4::CanQueueSubtitleEditBoxRequest(
		MakeRuntimeState(Automation4::MakeExportFilterRunInvocation("Edit box")), true));
	EXPECT_FALSE(Automation4::CanQueueSubtitleEditBoxRequest(
		MakeRuntimeState(Automation4::MakeExportFilterConfigInvocation("Edit box")), true));
}

TEST(SubtitleEditBoxFocusPolicy, requires_active_line) {
	EXPECT_FALSE(aegisub::subtitle_edit_box_focus::CanFocusEditControl(false, true, true));
}

TEST(SubtitleEditBoxFocusPolicy, requires_enabled_control) {
	EXPECT_FALSE(aegisub::subtitle_edit_box_focus::CanFocusEditControl(true, false, true));
}

TEST(SubtitleEditBoxFocusPolicy, requires_focus_accepting_control) {
	EXPECT_FALSE(aegisub::subtitle_edit_box_focus::CanFocusEditControl(true, true, false));
}

TEST(SubtitleEditBoxFocusPolicy, allows_focus_when_all_requirements_are_met) {
	EXPECT_TRUE(aegisub::subtitle_edit_box_focus::CanFocusEditControl(true, true, true));
}

}