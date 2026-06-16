#include "app_runtime_facilities.h"

#include "app_runtime.h"

#include "auto4_base.h"
#include "auto4_lua_factory.h"
#include "automation/engine/automation_engine_registry.h"
#include "automation/automation_debug_service.h"
#include "export_fixstyle.h"
#include "export_framerate.h"
#include "options.h"
#include "subtitles_provider_libass.h"

#include <libaegisub/exception.h>
#include <libaegisub/make_unique.h>

void InitializeRuntimeOptionalFacilities(AppRuntimeInitOptions const& options) {
	if (options.register_automation_script_factory) {
		if (!Automation4::AutomationEngineRegistry::FindEngine("Lua"))
			Automation4::AutomationEngineRegistry::Register(agi::make_unique<Automation4::LuaAutomationEngine>());
	}

	if (options.warm_subtitles_provider_font_cache)
		libass::CacheFonts();

	if (options.load_global_scripts)
		config::global_scripts = new Automation4::AutoloadScriptManager(OPT_GET("Path/Automation/Autoload")->GetString());

	if (options.shell_mode == RuntimeShellMode::Gui && !config::automation_debug_service)
		config::automation_debug_service = new Automation4::AutomationDebugService();

	if (options.register_export_filters) {
		AssExportFilterChain::Register(agi::make_unique<AssFixStylesFilter>());
		AssExportFilterChain::Register(agi::make_unique<AssTransformFramerateFilter>());
	}

	if (options.optional_facility_host.register_subtitle_format_extensions)
		options.optional_facility_host.register_subtitle_format_extensions();

	if (options.install_png_handler) {
		if (!options.optional_facility_host.install_png_image_handler)
			throw agi::InternalError("AppRuntime requested PNG handler installation without a host hook.");
		options.optional_facility_host.install_png_image_handler();
	}
}

void CleanupRuntimeOptionalFacilities() {
	if (config::global_scripts) {
		delete config::global_scripts;
		config::global_scripts = nullptr;
	}

	if (config::automation_debug_service) {
		delete config::automation_debug_service;
		config::automation_debug_service = nullptr;
	}

	AssExportFilterChain::Clear();
}
