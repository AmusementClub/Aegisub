// Windows GDI font selection authority.

#include "gdi_font_resolver.h"

#include "font_file_lister_dwrite.h"

#include <libaegisub/charset_conv_win.h>
#include <libaegisub/scoped_ptr.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dwrite.h>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

int QuantizeAssHeightForGdiProbe(double height) noexcept {
	if (!std::isfinite(height) || height <= 0.0)
		return 0;

	// Keep the conversion in floating point until after rounding so large
	// finite ASS values cannot overflow an intermediate integer conversion.
	auto rounded = std::floor(height + 0.5);
	if (!(rounded > 0.0))
		return 0;
	constexpr double max_height =
		static_cast<double>(std::numeric_limits<int>::max());
	if (rounded >= max_height)
		return -std::numeric_limits<int>::max();
	return -static_cast<int>(rounded);
}

namespace {

std::uint16_t ReadU16(std::span<std::uint8_t const> data, std::size_t offset, bool& ok) noexcept {
	if (offset > data.size() || data.size() - offset < 2) {
		ok = false;
		return 0;
	}
	return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8) | data[offset + 1]);
}

struct FontMetadata {
	int weight = 0;
	bool italic = false;
	bool oblique_bit = false;
	bool bold_bit = false;
	bool regular_bit = false;
	bool has_weight = false;
	bool has_selection = false;
	bool variable = false;
	bool malformed = false;
};

struct GdiStyleEvidence {
	bool enumerated = false;
	bool matching_role = false;
	bool truetype = false;
};

GdiStyleEvidence EnumerateGdiStyleEvidence(
	HDC dc,
	wchar_t const* family,
	int intrinsic_weight,
	bool intrinsic_italic,
	int charset) noexcept {
	GdiStyleEvidence result;
	if (!dc || !family || !*family)
		return result;

	struct State {
		int weight = 0;
		bool italic = false;
		GdiStyleEvidence evidence;
	} state{intrinsic_weight, intrinsic_italic, {}};

	LOGFONTW filter{};
	filter.lfCharSet = static_cast<BYTE>(std::clamp(charset, 0, 255));
	filter.lfOutPrecision = OUT_TT_PRECIS;
	filter.lfQuality = ANTIALIASED_QUALITY;
	filter.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	wcsncpy(filter.lfFaceName, family, LF_FACESIZE - 1);
	filter.lfFaceName[LF_FACESIZE - 1] = L'\0';

	EnumFontFamiliesExW(
		dc,
		&filter,
		[](LOGFONTW const* member, TEXTMETRICW const*, DWORD font_type, LPARAM data) -> int {
			auto* state = reinterpret_cast<State*>(data);
			if (!member)
				return 1;
			state->evidence.enumerated = true;
			if (member->lfWeight != state->weight ||
			    (member->lfItalic != 0) != state->italic)
				return 1;
			state->evidence.matching_role = true;
			if ((font_type & TRUETYPE_FONTTYPE) != 0)
				state->evidence.truetype = true;
			return 1;
		},
		reinterpret_cast<LPARAM>(&state),
		0);
	return state.evidence;
}

FontMetadata ReadGdiMetadata(HDC dc) noexcept {
	FontMetadata result;
	// GetFontData table tags use native DWORD byte order.
	if (GetFontData(dc, 0x72617666, 0, nullptr, 0) != GDI_ERROR) { // 'fvar'
		result.variable = true;
		return result;
	}
	constexpr DWORD os2 = 0x322f534f; // 'OS/2'
	auto const table_size = GetFontData(dc, os2, 0, nullptr, 0);
	if (table_size == GDI_ERROR || table_size == 0)
		return result;
	if (table_size < 6) {
		result.malformed = true;
		return result;
	}
	std::array<std::uint8_t, 64> data{};
	auto const bytes_to_read = static_cast<DWORD>(std::min<std::size_t>(data.size(), table_size));
	auto const bytes_read = GetFontData(dc, os2, 0, data.data(), bytes_to_read);
	if (bytes_read == GDI_ERROR || bytes_read != bytes_to_read) {
		result.malformed = true;
		return result;
	}
	auto const view = std::span<std::uint8_t const>(data).first(bytes_read);
	bool ok = true;
	auto const weight = ReadU16(view, 4, ok);
	auto const selection = view.size() >= 64 ? ReadU16(view, 62, ok) : 0;
	if (!ok) {
		result.malformed = true;
		return result;
	}
	if (weight >= 1 && weight <= 1000) {
		result.weight = weight;
		result.has_weight = true;
	}
	if (view.size() >= 64) {
		result.italic = (selection & 0x0001u) != 0;
		result.oblique_bit = (selection & 0x0200u) != 0;
		result.bold_bit = (selection & 0x0020u) != 0;
		result.regular_bit = (selection & 0x0040u) != 0;
		result.has_selection = true;
	}
	return result;
}

std::uint64_t HashBytes(
	std::span<std::uint8_t const> bytes,
	std::uint64_t hash = 1469598103934665603ull) noexcept {
	for (auto byte : bytes) {
		hash ^= byte;
		hash *= 1099511628211ull;
	}
	return hash ? hash : 1;
}

std::uint64_t HashInteger(std::uint64_t value, std::uint64_t hash) noexcept {
	std::array<std::uint8_t, sizeof(value)> bytes{};
	std::memcpy(bytes.data(), &value, sizeof(value));
	for (auto byte : bytes) {
		hash ^= byte;
		hash *= 1099511628211ull;
	}
	return hash;
}

std::uint64_t HashString(std::string_view value, std::uint64_t hash = 1469598103934665603ull) noexcept {
	for (auto byte : value) {
		hash ^= static_cast<unsigned char>(byte);
		hash *= 1099511628211ull;
	}
	return hash;
}

std::string LowerAscii(std::string_view value) {
	std::string result;
	result.reserve(value.size());
	for (auto ch : value)
		result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
	return result;
}

std::string MemoKey(
	std::string_view family,
	int weight,
	bool italic,
	int charset,
	int height) {
	return LowerAscii(family) + '\x1f' + std::to_string(weight) + '\x1f' +
	       (italic ? "1" : "0") + '\x1f' + std::to_string(charset) + '\x1f' +
	       std::to_string(height);
}

struct OrdinalIcaseLess {
	bool operator()(std::wstring const& left, std::wstring const& right) const noexcept {
		auto const result = CompareStringOrdinal(
			left.data(), static_cast<int>(left.size()),
			right.data(), static_cast<int>(right.size()), TRUE);
		if (result == CSTR_LESS_THAN)
			return true;
		if (result == CSTR_GREATER_THAN || result == CSTR_EQUAL)
			return false;
		return left < right;
	}
};

bool SameFamily(std::string_view requested, std::string_view selected) {
	auto strip_vertical = [](std::string_view value) {
		return !value.empty() && value.front() == '@' ? value.substr(1) : value;
	};
	auto const req = strip_vertical(requested);
	auto const got = strip_vertical(selected);
	if (req.empty() || got.empty())
		return false;
	auto const req_w = agi::charset::ConvertW(std::string(req));
	auto const got_w = agi::charset::ConvertW(std::string(got));
	if (req_w.empty() || got_w.empty())
		return false;
	return CompareStringOrdinal(req_w.data(), static_cast<int>(req_w.size()),
	                            got_w.data(), static_cast<int>(got_w.size()), TRUE)
		== CSTR_EQUAL;
}

struct SelectedFont {
	HFONT font = nullptr;
	HGDIOBJ previous = nullptr;
	HDC dc = nullptr;

	SelectedFont() = default;
	SelectedFont(SelectedFont const&) = delete;
	SelectedFont& operator=(SelectedFont const&) = delete;
	SelectedFont(SelectedFont&& other) noexcept
	: font(std::exchange(other.font, nullptr))
	, previous(std::exchange(other.previous, nullptr))
	, dc(std::exchange(other.dc, nullptr))
	{
	}
	~SelectedFont() {
		if (dc && previous)
			SelectObject(dc, previous);
		if (font)
			DeleteObject(font);
	}
};

std::uint64_t FingerprintGdiData(HDC dc) noexcept {
	constexpr DWORD max_font_bytes = 64u * 1024u * 1024u;
	constexpr std::size_t chunk_size = 16u * 1024u;
	DWORD table = 0x66637474; // GetFontData expects the byte-reversed 'ttcf' tag.
	auto size = GetFontData(dc, table, 0, nullptr, 0);
	if (size == GDI_ERROR) {
		table = 0;
		size = GetFontData(dc, table, 0, nullptr, 0);
	}
	if (size == GDI_ERROR || size == 0 || size > max_font_bytes)
		return 0;

	std::array<std::uint8_t, chunk_size> chunk{};
	std::uint64_t hash = 1469598103934665603ull;
	for (DWORD offset = 0; offset < size;) {
		auto const count = static_cast<DWORD>(
			std::min<std::uint64_t>(chunk.size(), static_cast<std::uint64_t>(size - offset)));
		auto const read = GetFontData(dc, table, offset, chunk.data(), count);
		if (read == GDI_ERROR || read != count)
			return 0;
		hash = HashBytes(std::span<std::uint8_t const>(chunk).first(count), hash);
		offset += count;
	}
	return HashInteger(size, hash);
}

struct ReleaseDWriteFace {
	void operator()(IDWriteFontFace *face) const noexcept {
		if (face)
			face->Release();
	}
};

using DWriteFontFacePtr = std::unique_ptr<IDWriteFontFace, ReleaseDWriteFace>;

} // namespace

struct GdiFontResolver::Impl {
	agi::scoped_holder<HDC, decltype(&DeleteDC)> dc{
		CreateCompatibleDC(nullptr), DeleteDC};
	std::unique_ptr<DWriteBridge> dwrite;
	std::unordered_map<std::string, GdiFontProbeResult> memo;
	GdiFontResolverStats stats;

	explicit Impl(GdiFontMetadataMode metadata_mode) {
		if (metadata_mode == GdiFontMetadataMode::SystemDWriteIfAvailable)
			dwrite = std::make_unique<DWriteBridge>(DWriteBridgeMode::SystemOnly);
	}

	SelectedFont Select(std::wstring const& family, int weight, bool italic, int charset, int height) {
		SelectedFont result;
		if (!dc || family.empty())
			return result;
		LOGFONTW lf{};
		lf.lfHeight = height;
		lf.lfWeight = weight;
		lf.lfItalic = italic ? TRUE : FALSE;
		lf.lfCharSet = static_cast<BYTE>(std::clamp(charset, 0, 255));
		lf.lfOutPrecision = OUT_TT_PRECIS;
		lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
		lf.lfQuality = ANTIALIASED_QUALITY;
		lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
		wcsncpy(lf.lfFaceName, family.c_str(), LF_FACESIZE - 1);
		lf.lfFaceName[LF_FACESIZE - 1] = L'\0';
		result.font = CreateFontIndirectW(&lf);
		if (!result.font)
			return result;
		result.previous = SelectObject(dc, result.font);
		if (!result.previous || result.previous == HGDI_ERROR) {
			DeleteObject(result.font);
			result.font = nullptr;
			result.previous = nullptr;
			return result;
		}
		result.dc = dc;
		return result;
	}
};

GdiFontResolver::GdiFontResolver(GdiFontMetadataMode metadata_mode)
: impl_(std::make_unique<Impl>(metadata_mode))
{
}

GdiFontResolver::~GdiFontResolver() = default;

bool GdiFontResolver::available() const noexcept {
	return impl_ && impl_->dc;
}

bool GdiFontResolver::dwrite_available() const noexcept {
	return impl_ && impl_->dwrite && impl_->dwrite->available();
}

std::string GdiFontResolver::dwrite_description() const {
	return dwrite_available() ? impl_->dwrite->dll_description() : std::string{};
}

GdiFontResolverStats GdiFontResolver::stats() const noexcept {
	return impl_ ? impl_->stats : GdiFontResolverStats{};
}

std::vector<std::string> GdiFontResolver::EnumerateFamilies() const {
	std::vector<std::string> result;
	if (!available())
		return result;
	struct State {
		std::vector<std::string> values;
		std::set<std::wstring, OrdinalIcaseLess> seen;
	} state;
	LOGFONTW lf{};
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_TT_PRECIS;
	lf.lfQuality = ANTIALIASED_QUALITY;
	lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	EnumFontFamiliesExW(impl_->dc, &lf,
		[](LOGFONTW const *font, TEXTMETRICW const *, DWORD, LPARAM data) -> int {
			auto *state = reinterpret_cast<State *>(data);
			if (!font || !font->lfFaceName[0] || font->lfFaceName[0] == L'@')
				return 1;
			std::wstring const key = font->lfFaceName;
			auto name = agi::charset::ConvertW(font->lfFaceName);
			if (!name.empty() && state->seen.insert(key).second)
				state->values.push_back(std::move(name));
			return 1;
		}, reinterpret_cast<LPARAM>(&state), 0);
	std::sort(state.values.begin(), state.values.end());
	return state.values;
}

GdiFontProbeResult GdiFontResolver::Probe(
	std::string_view family, int weight, bool italic, int charset, int height) {
	GdiFontProbeResult result;
	result.outcome.requested_weight = weight;
	result.outcome.requested_italic = italic;
	if (!available() || family.empty())
		return result;
	int const effective_charset = charset < 0
		? DEFAULT_CHARSET
		: std::clamp(charset, 0, 255);
	auto const memo_key = MemoKey(
		family, weight, italic, effective_charset, height);
	if (auto const it = impl_->memo.find(memo_key); it != impl_->memo.end()) {
		++impl_->stats.request_count;
		++impl_->stats.memo_hit_count;
		return it->second;
	}
	result = ProbeWithSelection(
		family, weight, italic, effective_charset, height, {});
	impl_->memo.insert_or_assign(memo_key, result);
	return result;
}

GdiFontProbeResult GdiFontResolver::ProbeWithSelection(
	std::string_view family,
	int weight,
	bool italic,
	int charset,
	int height,
	GdiFontSelectionCallback const& callback) {
	GdiFontProbeResult result;
	result.outcome.requested_weight = weight;
	result.outcome.requested_italic = italic;
	if (!available() || family.empty())
		return result;
	++impl_->stats.request_count;
	++impl_->stats.physical_probe_count;
	int const effective_charset = charset < 0
		? DEFAULT_CHARSET
		: std::clamp(charset, 0, 255);
	auto wide = agi::charset::ConvertW(std::string(family));
	if (wide.empty() || wide.size() >= LF_FACESIZE)
		return result;
	auto selected = impl_->Select(wide, weight, italic, effective_charset, height);
	if (!selected.font)
		return result;
	wchar_t selected_name[LF_FACESIZE] = {};
	if (GetTextFaceW(impl_->dc, LF_FACESIZE, selected_name) <= 0)
		return result;
	result.selected_family_name = agi::charset::ConvertW(selected_name);
	result.success = !result.selected_family_name.empty();

	TEXTMETRICW metrics{};
	GetTextMetricsW(impl_->dc, &metrics);
	result.outcome.realized_italic = metrics.tmItalic != 0;
	auto metadata = ReadGdiMetadata(impl_->dc);
	if (metadata.has_weight)
		result.outcome.realized_weight = metadata.weight;
	else if (metrics.tmWeight > 0)
		result.outcome.realized_weight = metrics.tmWeight;
	if (metadata.has_selection)
		result.outcome.realized_italic = metadata.italic || metadata.oblique_bit;
	auto const gdi_style = metadata.has_weight && metadata.has_selection
		? EnumerateGdiStyleEvidence(
			impl_->dc,
			selected_name,
			metadata.weight,
			metadata.italic || metadata.oblique_bit,
			effective_charset)
		: GdiStyleEvidence{};

	std::string path;
	int face_index = -1;
	DWriteFontFacePtr face;
	bool dwrite_synthetic = false;
	if (impl_->dwrite && impl_->dwrite->available()) {
		face.reset(impl_->dwrite->CreateFontFaceFromHdc(impl_->dc));
		if (face) {
			dwrite_synthetic = face->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE;
			impl_->dwrite->GetFontFilePath(face.get(), path, face_index);
			result.win32_family_names = impl_->dwrite->GetWin32FamilyNamesFromFace(face.get());
		}
	}

	// A file/path entity is useful for catalog identity, but is never exposed
	// through the snapshot or UI. Stream a bounded fingerprint only when system
	// DWrite cannot provide both a path and face index.
	std::uint64_t token = 0;
	bool const have_dwrite_entity = !path.empty() && face_index >= 0;
	if (have_dwrite_entity) {
		token = HashString(LowerAscii(path));
		token = HashString(std::to_string(face_index), token);
	}
	else {
		++impl_->stats.fingerprint_read_count;
		token = FingerprintGdiData(impl_->dc);
		if (token) {
			// GetFontData may return a complete TTC. Include the selected family so
			// different faces with identical RBIZ metadata do not share a token.
			token = HashString(LowerAscii(result.selected_family_name), token);
		}
	}
	if (token) {
		token = HashInteger(static_cast<std::uint64_t>(std::max(result.outcome.realized_weight, 0)), token);
		token = HashInteger(result.outcome.realized_italic ? 1u : 0u, token);
	}
	result.outcome.entity_token = token;

	result.matches_requested_family = SameFamily(family, result.selected_family_name);
	if (!result.matches_requested_family) {
		// GDI may return the localized spelling even when the request used an
		// English Win32 alias. These names came from this exact HDC face, so
		// accepting a matching alias does not weaken fallback detection.
		for (auto const& name : result.win32_family_names) {
			if (SameFamily(family, name.value)) {
				result.matches_requested_family = true;
				break;
			}
		}
	}
	bool const intrinsic_weight = metadata.has_weight &&
		(metadata.weight == 400 || metadata.weight == 700);
	bool const has_intrinsic_italic = metadata.has_selection;
	bool const metadata_conflict = metadata.has_selection &&
		((metadata.regular_bit &&
		  (metadata.bold_bit || metadata.italic || metadata.oblique_bit)) ||
		 (metadata.weight == 700 && (!metadata.bold_bit || metadata.regular_bit)) ||
		 (metadata.weight == 400 && metadata.bold_bit));
	// When DirectWrite is unavailable there is no simulation bit to consult. A
	// 700 request realized from a 400 face is still not a physical Bold entity,
	// so keep it diagnostic-only rather than guessing from the numeric weight.
	bool const synthetic_weight = weight == FW_BOLD && metadata.has_weight &&
		metadata.weight != FW_BOLD;
	bool const synthetic = dwrite_synthetic ||
		(italic && has_intrinsic_italic &&
		 !metadata.italic && !metadata.oblique_bit) || synthetic_weight;
	// Private AddFontMemResourceEx faces are selectable by CreateFontIndirectW
	// but are not consistently returned by EnumFontFamiliesExW. Treat a
	// positive enumeration as corroborating evidence; an actual conflicting
	// enumeration is unsafe, while absence leaves OS/2 + HDC identity in charge.
	bool const gdi_style_conflict = gdi_style.enumerated &&
		(!gdi_style.matching_role || !gdi_style.truetype);
	bool const noncanonical = !result.matches_requested_family || metadata.variable || metadata.malformed ||
		!intrinsic_weight || !has_intrinsic_italic || metadata_conflict ||
		gdi_style_conflict ||
		(!path.empty() && face_index < 0) ||
		(weight != FW_NORMAL && weight != FW_BOLD);
	if (synthetic || noncanonical || !token || !result.success) {
		result.outcome.status = !token ? FontVariantStatus::Unknown
			: synthetic ? FontVariantStatus::Synthetic
			: FontVariantStatus::NonCanonical;
		result.outcome.role = FontVariantRole::Unknown;
	}
	else {
		bool const bold = metadata.weight == 700;
		result.outcome.role = result.outcome.realized_italic
			? (bold ? FontVariantRole::BoldItalic : FontVariantRole::Italic)
			: (bold ? FontVariantRole::Bold : FontVariantRole::Regular);
		result.outcome.status = FontVariantStatus::Canonical;
	}

	if (callback) {
		callback(GdiFontSelectionView{
			impl_->dc, face.get(), impl_->dwrite.get(), &result, path, face_index});
	}
	// This entry point always performs a physical selection, but the fresh
	// result is authoritative for subsequent memoized requests on this private
	// resolver. Catalog alias validation can therefore reuse the exact outcome
	// it just observed without selecting the same HFONT again.
	impl_->memo.insert_or_assign(
		MemoKey(family, weight, italic, effective_charset, height), result);
	return result;
}

FontFamilyVariantProfile GdiFontResolver::BuildProfile(
	std::string_view family, int charset, int height) {
	std::array<FontVariantOutcome, 4> outcomes;
	bool profile_matches_requested_family = true;
	for (std::size_t index = 0; index < outcomes.size(); ++index) {
		bool const italic = index >= 2;
		bool const bold = (index & 1u) != 0;
		auto probe = Probe(
			family, bold ? FW_BOLD : FW_NORMAL, italic, charset, height);
		outcomes[index] = probe.outcome;
		profile_matches_requested_family = profile_matches_requested_family &&
			probe.success && probe.matches_requested_family &&
			probe.outcome.entity_token != 0;
	}
	return BuildFontFamilyVariantProfile(
		std::move(outcomes), FontVariantBackend::VsFilterGdi,
		FontSelectionEvidence::Observed,
		profile_matches_requested_family);
}
