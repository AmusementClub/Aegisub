#pragma once

#include "lsmas_native_api.generated.h"

#include <string>

namespace lsmas {

struct Api {
#define AGI_LSMAS_REQUIRED(symbol, member) decltype(&symbol) member = nullptr;
#define AGI_LSMAS_OPTIONAL(symbol, member) decltype(&symbol) member = nullptr;
#include "lsmas_native_api.functions.inc"
#undef AGI_LSMAS_OPTIONAL
#undef AGI_LSMAS_REQUIRED
};

void EnsureLoaded();
bool IsAvailable() noexcept;
std::string GetLoadError();
std::string GetLoadedLibrary();
Api const& GetApi();

}
