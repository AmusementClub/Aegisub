#include "font_family_catalog_source.h"

#include <utility>

namespace {

class PlatformFontFamilyCatalogSource final : public IFontFamilyCatalogSource {
	FontFamilyCatalogSourceInfo info;

public:
	PlatformFontFamilyCatalogSource() {
#if defined(_WIN32)
		info.backend = FontVariantBackend::VsFilterGdi;
		info.provider = "gdi";
		info.authoritative = true;
#else
		// BuildFontFamilyCatalog() is intentionally empty on unsupported hosts at
		// present. Keep the identity explicit so callers can distinguish an
		// unavailable native source from a libass score or an observed match.
		info.backend = FontVariantBackend::Unknown;
		info.provider = "unavailable";
		info.authoritative = false;
#endif
	}

	FontFamilyCatalogSourceInfo const& Info() const noexcept override {
		return info;
	}

	FontFamilyCatalog Build() override {
		return BuildFontFamilyCatalog();
	}
};

} // namespace

FontFamilyCatalogSourcePtr CreatePlatformFontFamilyCatalogSource() {
	return std::make_shared<PlatformFontFamilyCatalogSource>();
}
