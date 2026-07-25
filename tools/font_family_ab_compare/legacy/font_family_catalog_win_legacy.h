// Snapshot of the pre-observation Windows catalog builder (HEAD at extraction).
// Used only by tools/font_family_ab_compare for A/B equivalence evidence.
#pragma once

#ifndef _WIN32
#error "font_family_catalog_win_legacy.h is Windows-only"
#endif

#include "font_family_catalog.h"

/// Build a catalog with the historical single-pass GDI builder. Does not touch
/// the observation store or repository path. entity_token values are process-
/// local to the resolver created inside this call.
FontFamilyCatalog BuildFontFamilyCatalogLegacy();
