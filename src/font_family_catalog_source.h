#pragma once

#include "font_family_catalog.h"

#include <memory>
#include <string>

/// Describes the authority which enumerates a family catalog.
///
/// This is deliberately separate from cache generation. A source identifies
/// the matching/enumeration backend, while the cache owns only lifetime and
/// publication of immutable snapshots.
struct FontFamilyCatalogSourceInfo {
	FontVariantBackend backend = FontVariantBackend::Unknown;
	std::string provider;
	bool authoritative = false;

	bool operator==(FontFamilyCatalogSourceInfo const&) const = default;
};

/// Platform-neutral family catalog construction seam.
///
/// Implementations may own backend-specific state (for example a GDI HDC or a
/// CoreText/fontconfig session), but must not expose that state through the
/// catalog. The cache invokes Build() outside its coordination mutex.
class IFontFamilyCatalogSource {
public:
	virtual ~IFontFamilyCatalogSource() = default;

	/// Stable metadata for diagnostics and backend-aware policy decisions.
	virtual FontFamilyCatalogSourceInfo const& Info() const noexcept = 0;

	/// Enumerate and construct one immutable catalog value.
	virtual FontFamilyCatalog Build() = 0;
};

using FontFamilyCatalogSourcePtr = std::shared_ptr<IFontFamilyCatalogSource>;

/// Construct the source used by the current platform build.
///
/// Windows currently returns the VSFilter/GDI source. Other platforms return
/// an explicit unavailable/unknown source until a native catalog provider is
/// installed; this keeps common cache and UI code free of platform ifdefs.
FontFamilyCatalogSourcePtr CreatePlatformFontFamilyCatalogSource();
