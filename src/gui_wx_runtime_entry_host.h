#pragma once

#include "app_runtime.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct GuiWxRuntimeEntryHostPack {
	AppRuntimeMainQueueHooks main_queue_hooks;
	std::shared_ptr<UiTimerHost> ui_timer_host;
	RuntimeLocaleHost locale_host;
	RuntimeProcessHost process_host;
	RuntimeOptionalFacilityHost optional_facility_host;
	RuntimeBootstrapUiHost bootstrap_ui_host;
};

// Replace this pack provider when a non-wx GUI shell needs to supply the full
// runtime bring-up host bundle consumed by the GUI entry point.
GuiWxRuntimeEntryHostPack BuildGuiWxRuntimeEntryHostPack();

// Replace this helper when a non-wx GUI shell needs to define the runtime
// initialization policy consumed by the GUI entry point.
AppRuntimeInitOptions BuildGuiWxAppRuntimeInitOptions();

// Replace these helpers when a non-wx GUI shell needs bootstrap UI access
// before a project context exists.
agi::InteractionResult RequestGuiWxBootstrapUiInteraction(agi::InteractionRequest const& request);
void ShowGuiWxBootstrapUiError(std::string const& title, std::string const& message);

// Replace these helpers when a non-wx GUI shell needs to own startup
// orchestration and main-queue exception binding.
void BindGuiWxMainQueueDispatchHandler(std::function<void()> on_exception);
void RunGuiWxAppStartupSequence(
	std::vector<std::string> const& args,
	std::function<void()> create_project_context,
	std::function<void(std::vector<std::string> const&)> open_files,
	bool reload_global_scripts = true);
