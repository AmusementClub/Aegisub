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
