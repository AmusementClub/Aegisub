#pragma once

#include "app_runtime.h"

struct GuiWxRuntimeEntryHostPack {
	AppRuntimeMainQueueHooks main_queue_hooks;
	RuntimeLocaleHost locale_host;
	RuntimeProcessHost process_host;
	RuntimeOptionalFacilityHost optional_facility_host;
	RuntimeBootstrapUiHost bootstrap_ui_host;
};

// Replace this pack provider when a non-wx GUI shell needs to supply the full
// runtime bring-up host bundle consumed by the GUI entry point.
GuiWxRuntimeEntryHostPack BuildGuiWxRuntimeEntryHostPack();
