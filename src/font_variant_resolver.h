#pragma once

#include "ass_font_state.h"
#include "font_family_catalog.h"

#include <memory>
#include <optional>
#include <string>

struct FontVariantResolverCapabilities {
	bool confirms_physical_entity = false;
	bool supports_automatic_pinning = false;
};

struct FontVariantResolverInfo {
	FontVariantBackend backend = FontVariantBackend::Unknown;
	FontSelectionEvidence evidence = FontSelectionEvidence::None;
	FontVariantResolverCapabilities capabilities;
	std::string provider;
	std::string algorithm;
};

/// Platform-neutral family/variant selection interface. Implementations model
/// one named selection authority (GDI, CoreText, Fontconfig, libass scoring),
/// and must describe whether their result is observed or only predicted.
class FontVariantResolver {
public:
	virtual ~FontVariantResolver() = default;

	virtual FontVariantResolverInfo const& Info() const noexcept = 0;
	virtual bool Available() const noexcept = 0;
	virtual FontVariantOutcome Resolve(
		aegisub::ass::AssFontRequest const& request) = 0;

	/// Build physical family choices. The height remains a double at this
	/// platform-neutral boundary; a backend quantizes it only when its native
	/// selection API requires an integer request.
	virtual FontFamilyVariantProfile BuildProfile(
		std::string const& family,
		int charset,
		double height = 0.0);
};

/// Native resolver for the current platform. Unsupported platforms receive an
/// unavailable resolver so common UI and audit code need no platform ifdefs.
std::unique_ptr<FontVariantResolver> CreatePlatformFontVariantResolver();

/// Build a live profile for a catalog record which was already selected by
/// stable family id. This avoids resolving a potentially ambiguous display
/// alias a second time.
std::optional<FontFamilyVariantProfile> BuildFontVariantProfileForFamily(
	FontVariantResolver& resolver,
	FontFamilyRecord const& family,
	int charset,
	double height = 0.0);

/// Resolve the request's alias through a catalog, then build a request-specific
/// family profile with the supplied backend.
std::optional<FontFamilyVariantProfile> BuildFontVariantProfileForRequest(
	FontVariantResolver& resolver,
	FontFamilyCatalog const& catalog,
	aegisub::ass::AssFontRequest const& request);
