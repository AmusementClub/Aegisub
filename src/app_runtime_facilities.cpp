// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include "app_runtime_facilities.h"

#include "app_runtime.h"

#include "auto4_base.h"
#include "auto4_lua_factory.h"
#include "export_fixstyle.h"
#include "export_framerate.h"
#include "options.h"
#include "subtitles_provider_libass.h"

#include <libaegisub/exception.h>
#include <libaegisub/make_unique.h>

void InitializeRuntimeOptionalFacilities(AppRuntimeInitOptions const& options) {
	if (options.register_automation_script_factory)
		Automation4::ScriptFactory::Register(agi::make_unique<Automation4::LuaScriptFactory>());

	if (options.warm_subtitles_provider_font_cache)
		libass::CacheFonts();

	if (options.load_global_scripts)
		config::global_scripts = new Automation4::AutoloadScriptManager(OPT_GET("Path/Automation/Autoload")->GetString());

	if (options.register_export_filters) {
		AssExportFilterChain::Register(agi::make_unique<AssFixStylesFilter>());
		AssExportFilterChain::Register(agi::make_unique<AssTransformFramerateFilter>());
	}

	if (options.install_png_handler) {
		if (!options.host_hooks.install_png_image_handler)
			throw agi::InternalError("AppRuntime requested PNG handler installation without a host hook.");
		options.host_hooks.install_png_image_handler();
	}
}

void CleanupRuntimeOptionalFacilities() {
	if (config::global_scripts) {
		delete config::global_scripts;
		config::global_scripts = nullptr;
	}

	AssExportFilterChain::Clear();
}
