#include "gui_wx_runtime_entry_host.h"

#include "gui_wx_bootstrap_ui_host.h"
#include "gui_wx_dispatch_event.h"
#include "gui_wx_locale_host.h"
#include "gui_wx_runtime_host.h"

#include <wx/app.h>
#include <wx/thread.h>

namespace {

AppRuntimeMainQueueHooks BuildGuiWxRuntimeMainQueueHooks() {
	return {
		[](agi::dispatch::Thunk thunk) {
			auto evt = new ValueEvent<agi::dispatch::Thunk>(EVT_CALL_THUNK, -1, std::move(thunk));
			wxTheApp->QueueEvent(evt);
		},
		[] {
			return wxIsMainThread();
		},
		{}
	};
}

}

GuiWxRuntimeEntryHostPack BuildGuiWxRuntimeEntryHostPack() {
	GuiWxRuntimeEntryHostPack hosts;
	hosts.main_queue_hooks = BuildGuiWxRuntimeMainQueueHooks();
	hosts.locale_host = BuildGuiWxRuntimeLocaleHost();
	hosts.process_host = BuildGuiWxRuntimeProcessHost();
	hosts.optional_facility_host = BuildGuiWxRuntimeOptionalFacilityHost();
	hosts.bootstrap_ui_host = BuildGuiWxRuntimeBootstrapUiHost();
	return hosts;
}

AppRuntimeInitOptions BuildGuiWxAppRuntimeInitOptions() {
	auto gui_runtime_hosts = BuildGuiWxRuntimeEntryHostPack();
	AppRuntimeInitOptions options;
	options.shell_mode = RuntimeShellMode::Gui;
	options.locale_policy = RuntimeLocalePolicy::PickIfNeeded;
	options.main_queue_hooks = std::move(gui_runtime_hosts.main_queue_hooks);
	options.locale_host = std::move(gui_runtime_hosts.locale_host);
	options.load_global_scripts = true;
	options.initialize_commands = true;
	options.initialize_ui_locale = true;
	options.register_automation_script_factory = true;
	options.warm_subtitles_provider_font_cache = true;
	options.register_export_filters = true;
	options.install_png_handler = true;
	options.process_host = std::move(gui_runtime_hosts.process_host);
	options.optional_facility_host = std::move(gui_runtime_hosts.optional_facility_host);
	options.bootstrap_ui_host = gui_runtime_hosts.bootstrap_ui_host;
	return options;
}

agi::InteractionResult RequestGuiWxBootstrapUiInteraction(agi::InteractionRequest const& request) {
	return RequestBootstrapUiInteraction(BuildGuiWxRuntimeEntryHostPack().bootstrap_ui_host, request);
}

void ShowGuiWxBootstrapUiError(std::string const& title, std::string const& message) {
	ShowBootstrapUiError(BuildGuiWxRuntimeEntryHostPack().bootstrap_ui_host, title, message);
}
