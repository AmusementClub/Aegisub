#pragma once

#include "runtime_locale_host.h"

// Replace this hook provider when a non-wx GUI shell needs to supply
// translation bring-up and preferred-language resolution.
RuntimeLocaleHost BuildGuiWxRuntimeLocaleHost();
