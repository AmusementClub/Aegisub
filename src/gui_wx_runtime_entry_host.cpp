#include "gui_wx_runtime_entry_host.h"

#include "auto4_base.h"
#include "compat.h"
#include "dialogs.h"
#include "gui_wx_bootstrap_ui_host.h"
#include "gui_wx_dispatch_event.h"
#include "gui_wx_locale_host.h"
#include "gui_wx_runtime_host.h"
#include "options.h"
#include "perf_trace.h"

#include <chrono>
#include <wx/app.h>
#include <wx/arrstr.h>
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
	hosts.ui_timer_host = CreateGuiWxUiTimerHost();
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
	options.ui_timer_host = std::move(gui_runtime_hosts.ui_timer_host);
	options.locale_host = std::move(gui_runtime_hosts.locale_host);
	options.create_global_script_manager = true;
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

void BindGuiWxMainQueueDispatchHandler(std::function<void()> on_exception) {
	wxTheApp->Bind(EVT_CALL_THUNK, [on_exception = std::move(on_exception)](ValueEvent<agi::dispatch::Thunk>& evt) mutable {
		try {
			evt.Get()();
		}
		catch (...) {
			on_exception();
		}
	});
}

void RunGuiWxAppStartupSequence(
	std::vector<std::string> const& args,
	std::function<void()> create_project_context,
	std::function<void(std::vector<std::string> const&)> open_files,
	bool reload_global_scripts) {
	auto phase_started = std::chrono::steady_clock::now();
	auto observe_phase = [&](char const* phase) {
		perf_trace::ObserveWindowOpenPhase(
			"main",
			phase,
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_started).count());
		phase_started = std::chrono::steady_clock::now();
	};

	create_project_context();
	observe_phase("startup.sequence.create_project_context");

	if (OPT_GET("App/First Start")->GetBool()) {
		OPT_SET("App/First Start")->SetBool(false);
#ifdef WITH_UPDATE_CHECKER
		auto request = agi::InteractionRequest{
			from_wx(_("Check for updates?")),
			from_wx(_("Do you want Aegisub to check for updates whenever it starts? You can still do it manually via the Help menu.")),
			agi::InteractionButtons::YesNo,
			agi::InteractionIcon::Question
		};
		auto result = RequestGuiWxBootstrapUiInteraction(request);
		OPT_SET("App/Auto/Check For Updates")->SetBool(result == agi::InteractionResult::Yes);
		try {
			config::opt->Flush();
		}
		catch (agi::fs::FileSystemError const& e) {
			ShowGuiWxBootstrapUiError("Error saving config file", e.GetMessage());
		}
#endif
	}
	observe_phase("startup.sequence.first_start_prompt");

#ifdef WITH_UPDATE_CHECKER
	PerformVersionCheck(false);
#endif
	observe_phase("startup.sequence.update_check.schedule");

	if (args.size() > 1) {
		std::vector<std::string> startup_files;
		startup_files.reserve(args.size() - 1);
		for (size_t i = 1; i < args.size(); ++i)
			startup_files.emplace_back(args[i]);
		open_files(startup_files);
	}
	observe_phase("startup.sequence.open_files");

	if (reload_global_scripts && config::global_scripts)
		config::global_scripts->ReloadAsync();
	observe_phase("startup.sequence.global_scripts.reload_async_schedule");
}
