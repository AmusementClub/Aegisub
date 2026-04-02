#pragma once

#include "runtime_optional_facility_host.h"
#include "runtime_process_host.h"

#include <memory>

class UiTimerHost;

// Replace this host provider when a non-wx GUI shell needs to supply
// process-level runtime bring-up services.
RuntimeProcessHost BuildGuiWxRuntimeProcessHost();

// Replace this host provider when a non-wx GUI shell needs to supply
// optional runtime facility hooks.
RuntimeOptionalFacilityHost BuildGuiWxRuntimeOptionalFacilityHost();

// Replace this host provider when a non-wx GUI shell needs to supply
// dispatcher-owned UI timers for shared controller playback cadence.
std::shared_ptr<UiTimerHost> CreateGuiWxUiTimerHost();
