// Windows GDI font selection authority used by the family catalog.
//
// This interface deliberately exposes only lightweight names and opaque face
// tokens. Resolved file paths never become part of a catalog snapshot or UI
// model.
#pragma once

#ifndef _WIN32
#error "gdi_font_resolver.h is Windows-only"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "font_family_catalog.h"
#include "font_file_lister_dwrite.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct GdiFontProbeResult {
	FontVariantOutcome outcome;
	/// The face name returned by GetTextFaceW; useful for fallback diagnostics.
	/// This is a family name, never a file path.
	std::string selected_family_name;
	std::vector<DWriteLocalizedName> win32_family_names;
	/// Whether the selected physical face belongs to the requested family or
	/// one of its Win32 aliases. False means GDI substituted another family.
	bool matches_requested_family = false;
	bool success = false;
};

/// Borrowed handles for collecting data from the exact physical face selected
/// by a resolver probe. The handles are valid only for the duration of the
/// ProbeWithSelection callback and must not be retained or released.
struct GdiFontSelectionView {
	HDC dc = nullptr;
	IDWriteFontFace *dwrite_face = nullptr;
	DWriteBridge const *dwrite_bridge = nullptr;
	GdiFontProbeResult const *probe = nullptr;
	/// Collector-only local entity data. This is intentionally absent from
	/// GdiFontProbeResult and therefore cannot enter catalog/UI snapshots.
	std::string_view local_file_path;
	int face_index = -1;
};

using GdiFontSelectionCallback =
	std::function<void(GdiFontSelectionView const&)>;

struct GdiFontResolverStats {
	std::uint64_t request_count = 0;
	std::uint64_t physical_probe_count = 0;
	std::uint64_t memo_hit_count = 0;
	std::uint64_t fingerprint_read_count = 0;
};

/// Convert an ASS font size to the representative LOGFONT character height
/// used by live GDI probes. ASS sizes are doubles, while LOGFONT accepts an
/// integer height; without a renderer scale context this is deliberately only
/// a stable probe quantization, not an exact xy-VSFilter render height.
/// Finite positive values are rounded as floor(value + 0.5), represented as a
/// negative character height, and saturated to -INT_MAX. Non-positive and
/// non-finite values use GDI's default height (0).
int QuantizeAssHeightForGdiProbe(double height) noexcept;

/// Metadata source used after GDI has selected the physical font.
enum class GdiFontMetadataMode : std::uint8_t {
	SystemDWriteIfAvailable,
	GdiOnly
};

class GdiFontResolver {
public:
	explicit GdiFontResolver(
		GdiFontMetadataMode metadata_mode =
			GdiFontMetadataMode::SystemDWriteIfAvailable);
	~GdiFontResolver();

	GdiFontResolver(GdiFontResolver const&) = delete;
	GdiFontResolver& operator=(GdiFontResolver const&) = delete;

	bool available() const noexcept;
	bool dwrite_available() const noexcept;
	std::string dwrite_description() const;
	GdiFontResolverStats stats() const noexcept;

	/// Enumerate the selectable family names from this resolver's private HDC.
	std::vector<std::string> EnumerateFamilies() const;

	/// Probe the physical face selected by GDI for a family/request pair.
	/// `weight` is the exact LOGFONT request (normally 400 or 700).
	GdiFontProbeResult Probe(std::string_view family,
	                         int weight = FW_NORMAL,
	                         bool italic = false,
	                         int charset = DEFAULT_CHARSET,
	                         int height = 0);

	/// Bypass any cached result and expose the selected HDC/face to a bounded
	/// callback. The fresh result replaces this request's memo entry. Collector
	/// path, memory-font, and glyph inspection must use this method so all
	/// observations come from the same GDI selection.
	GdiFontProbeResult ProbeWithSelection(
		std::string_view family,
		int weight,
		bool italic,
		int charset,
		int height,
		GdiFontSelectionCallback const& callback);

	/// Probe the four canonical requests and derive safe UI choices.
	/// `height` is already the quantized negative LOGFONT character height;
	/// callers holding an ASS double should use QuantizeAssHeightForGdiProbe.
	FontFamilyVariantProfile BuildProfile(std::string_view family,
	                                      int charset = DEFAULT_CHARSET,
	                                      int height = 0);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
