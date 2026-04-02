#pragma once

#include "runtime_bootstrap_ui_host.h"

// Replace this host provider when a non-wx GUI shell needs bootstrap UI
// services before a project context exists.
RuntimeBootstrapUiHost BuildGuiWxRuntimeBootstrapUiHost();
