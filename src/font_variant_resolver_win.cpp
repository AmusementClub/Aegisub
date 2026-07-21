#include "font_variant_resolver.h"

#include "gdi_font_resolver.h"

#include <memory>

namespace {

class GdiFontVariantResolver final : public FontVariantResolver {
	GdiFontResolver resolver;
	FontVariantResolverInfo info;

public:
	GdiFontVariantResolver() {
		info.backend = FontVariantBackend::VsFilterGdi;
		info.evidence = FontSelectionEvidence::Observed;
		info.capabilities.confirms_physical_entity = true;
		info.capabilities.supports_automatic_pinning = true;
		info.provider = "Windows GDI";
		info.algorithm = "VSFilter-compatible LOGFONT selection";
	}

	FontVariantResolverInfo const& Info() const noexcept override { return info; }
	bool Available() const noexcept override { return resolver.available(); }

	FontVariantOutcome Resolve(
		aegisub::ass::AssFontRequest const& request) override {
		if (!request.valid || request.family.empty())
			return {};
		return resolver.Probe(
			request.family,
			request.effective_weight,
			request.italic,
			request.charset,
			QuantizeAssHeightForGdiProbe(request.height)).outcome;
	}

	FontFamilyVariantProfile BuildProfile(
		std::string const& family,
		int charset,
		double height) override {
		return resolver.BuildProfile(
			family, charset, QuantizeAssHeightForGdiProbe(height));
	}
};

} // namespace

std::unique_ptr<FontVariantResolver> CreatePlatformFontVariantResolver() {
	return std::make_unique<GdiFontVariantResolver>();
}
