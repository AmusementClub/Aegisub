#pragma once

#include "app_runtime.h"

// Replace this hook provider when a non-wx GUI shell needs to supply
// AppRuntime bring-up services.
AppRuntimeHostHooks BuildGuiWxRuntimeHostHooks();
