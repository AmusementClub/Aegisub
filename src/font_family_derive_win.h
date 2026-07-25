#pragma once

#ifndef _WIN32
#error "font_family_derive_win.h is Windows-only"
#endif

#include "font_family_catalog.h"
#include "font_family_obs_types.h"

#include <string_view>

/// Pure replay of Windows catalog observations. It must never call GDI,
/// DirectWrite, or GdiFontResolver; alias safety is determined exclusively by
/// FontFamilyAliasObservation facts captured during observation.
FontFamilyCatalog DeriveWindowsFontFamilyCatalog(
	FontFamilyCatalogObservations const& observations,
	std::string_view locale);
