#pragma once

#include "runtime_optional_facility_host.h"
#include "runtime_process_host.h"

// Replace this host provider when a non-wx GUI shell needs to supply
// process-level runtime bring-up services.
RuntimeProcessHost BuildGuiWxRuntimeProcessHost();

// Replace this host provider when a non-wx GUI shell needs to supply
// optional runtime facility hooks.
RuntimeOptionalFacilityHost BuildGuiWxRuntimeOptionalFacilityHost();
