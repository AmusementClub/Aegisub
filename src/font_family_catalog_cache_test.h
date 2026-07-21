// Test seam for deterministic cache concurrency tests. Production callers
// should include font_family_catalog_cache.h instead.
#pragma once

#include "font_family_catalog_source.h"

#include <functional>

namespace font_family_catalog_cache::testing {

using CatalogBuilder = std::function<FontFamilyCatalog()>;

/// Install a source after resetting outstanding cache work. This is intended
/// for deterministic tests and host integration; production code should use
/// font_family_catalog_cache::SetSource().
void SetSource(FontFamilyCatalogSourcePtr source);

/// Wait for and discard all cache work, without changing the current builder.
void Reset();

/// Install a deterministic builder after resetting outstanding cache work.
void SetBuilder(CatalogBuilder builder);

/// Reset outstanding work and restore BuildFontFamilyCatalog as the builder.
void RestoreDefaultBuilder();

} // namespace font_family_catalog_cache::testing
