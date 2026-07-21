#include "../../src/gdi_font_resolver.h"
#include "../../src/font_file_lister.h"
#include "../../src/font_variant_policy.h"
#include "../../src/font_variant_resolver.h"

#include <libaegisub/charset_conv_win.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

int Fail(char const* message) {
	std::cerr << "gdi-font-resolver-smoke: " << message << '\n';
	return 1;
}

int Fail(std::string const& message) {
	return Fail(message.c_str());
}

int ValidateHeightQuantization() {
	struct Case {
		double input;
		int expected;
	};
	for (auto const& test : std::array{
			Case{0.0, 0}, Case{0.4, 0}, Case{12.4, -12},
			Case{12.5, -13}, Case{12.6, -13}, Case{-1.0, 0},
			Case{std::numeric_limits<double>::quiet_NaN(), 0},
			Case{std::numeric_limits<double>::infinity(), 0},
			Case{-std::numeric_limits<double>::infinity(), 0}}) {
		if (QuantizeAssHeightForGdiProbe(test.input) != test.expected)
			return Fail("ASS height quantization does not match the GDI probe policy");
	}
	if (QuantizeAssHeightForGdiProbe(
			std::numeric_limits<double>::max()) != -std::numeric_limits<int>::max())
		return Fail("large ASS height was not saturated before GDI conversion");
	return 0;
}

DWORD GdiObjectCount() {
	return GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
}

bool SameOutcome(FontVariantOutcome const& left, FontVariantOutcome const& right) {
	return left.requested_weight == right.requested_weight &&
	       left.requested_italic == right.requested_italic &&
	       left.realized_weight == right.realized_weight &&
	       left.realized_italic == right.realized_italic &&
	       left.role == right.role &&
	       left.status == right.status &&
	       left.entity_token == right.entity_token;
}

bool SameProfile(
	FontFamilyVariantProfile const& left,
	FontFamilyVariantProfile const& right) {
	if (left.backend != right.backend || left.evidence != right.evidence ||
	    left.automatic_pinning_reliable != right.automatic_pinning_reliable)
		return false;
	for (std::size_t index = 0; index < left.outcomes.size(); ++index) {
		if (!SameOutcome(left.outcomes[index], right.outcomes[index]))
			return false;
	}
	return true;
}

bool IsFullyCanonical(FontFamilyVariantProfile const& profile) {
	return std::all_of(
		profile.outcomes.begin(), profile.outcomes.end(),
		[](FontVariantOutcome const& outcome) {
			return outcome.status == FontVariantStatus::Canonical &&
			       outcome.entity_token != 0;
		});
}

int ValidateInstalledCatalog() {
	auto catalog = BuildFontFamilyCatalog();
	if (catalog.empty())
		return Fail("installed GDI family catalog is empty");
	std::unordered_set<FontFamilyId> ids;
	for (std::size_t index = 0; index < catalog.records().size(); ++index) {
		auto const& record = catalog.records()[index];
		if (record.id == 0 || !ids.insert(record.id).second ||
		    catalog.Find(record.id) != &record)
			return Fail("installed GDI family catalog has an invalid id index");
		if (!IsFullyCanonical(record.variant_profile))
			continue;
		for (std::size_t prior = 0; prior < index; ++prior) {
			auto const& candidate = catalog.records()[prior];
			if (IsFullyCanonical(candidate.variant_profile) &&
			    SameProfile(candidate.variant_profile, record.variant_profile))
				return Fail("catalog published duplicate canonical physical families");
		}
	}
	return 0;
}

class MemoryFont {
	std::vector<char> bytes;
	HANDLE handle = nullptr;

public:
	MemoryFont() = default;
	MemoryFont(MemoryFont const&) = delete;
	MemoryFont& operator=(MemoryFont const&) = delete;

	~MemoryFont() {
		if (handle)
			RemoveFontMemResourceEx(handle);
	}

	bool Load(std::filesystem::path const& path) {
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream)
			return false;
		auto const size = stream.tellg();
		if (size <= 0 || size > static_cast<std::streamoff>(MAXDWORD))
			return false;
		bytes.resize(static_cast<std::size_t>(size));
		stream.seekg(0);
		if (!stream.read(bytes.data(), size))
			return false;
		DWORD font_count = 0;
		handle = AddFontMemResourceEx(
			bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, &font_count);
		return handle && font_count != 0;
	}
};

struct DirectGdiSelection {
	LOGFONTW logical_font{};
	TEXTMETRICW metrics{};
	std::string selected_family_name;
	std::uint64_t font_data_fingerprint = 0;
	int intrinsic_weight = 0;
	bool intrinsic_italic = false;
	bool has_intrinsic_metadata = false;
	bool success = false;
};

std::uint64_t FingerprintSelectedFont(HDC dc) {
	if (!dc)
		return 0;
	constexpr DWORD max_font_bytes = 64u * 1024u * 1024u;
	constexpr DWORD ttcf_tag = 0x66637474; // GetFontData's byte-reversed 'ttcf'.
	constexpr std::size_t chunk_size = 16u * 1024u;
	DWORD table = ttcf_tag;
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
		auto const count = static_cast<DWORD>(std::min<std::uint64_t>(
			chunk.size(), static_cast<std::uint64_t>(size - offset)));
		auto const read = GetFontData(dc, table, offset, chunk.data(), count);
		if (read == GDI_ERROR || read != count)
			return 0;
		for (auto byte : std::span<std::uint8_t const>(chunk).first(count)) {
			hash ^= byte;
			hash *= 1099511628211ull;
		}
		offset += count;
	}
	return hash ? hash : 1;
}

std::uint16_t ReadBigEndianU16(
	std::array<std::uint8_t, 64> const& bytes,
	std::size_t offset) {
	return static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(bytes[offset]) << 8) |
		bytes[offset + 1]);
}

DirectGdiSelection ProbeDirectGdi(
	std::string_view family,
	int weight,
	bool italic,
	int charset,
	int height) {
	DirectGdiSelection result;
	auto const wide_family = agi::charset::ConvertW(std::string(family));
	if (wide_family.empty() || wide_family.size() >= LF_FACESIZE)
		return result;

	HDC const dc = CreateCompatibleDC(nullptr);
	if (!dc)
		return result;
	LOGFONTW request{};
	request.lfHeight = height;
	request.lfWeight = weight;
	request.lfItalic = italic ? TRUE : FALSE;
	request.lfCharSet = static_cast<BYTE>(std::clamp(charset, 0, 255));
	request.lfOutPrecision = OUT_TT_PRECIS;
	request.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	request.lfQuality = ANTIALIASED_QUALITY;
	request.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	wcsncpy(request.lfFaceName, wide_family.c_str(), LF_FACESIZE - 1);
	request.lfFaceName[LF_FACESIZE - 1] = L'\0';

	HFONT const font = CreateFontIndirectW(&request);
	if (!font) {
		DeleteDC(dc);
		return result;
	}
	HGDIOBJ const previous = SelectObject(dc, font);
	if (!previous || previous == HGDI_ERROR) {
		DeleteObject(font);
		DeleteDC(dc);
		return result;
	}

	wchar_t selected_name[LF_FACESIZE] = {};
	bool const have_logfont =
		GetObjectW(font, static_cast<int>(sizeof(result.logical_font)),
			&result.logical_font) ==
			static_cast<int>(sizeof(result.logical_font));
	bool const have_name =
		GetTextFaceW(dc, LF_FACESIZE, selected_name) > 0;
	bool const have_metrics = GetTextMetricsW(dc, &result.metrics) != FALSE;
	if (have_name)
		result.selected_family_name = agi::charset::ConvertW(selected_name);
	result.font_data_fingerprint = FingerprintSelectedFont(dc);

	constexpr DWORD os2 = 0x322f534f; // 'OS/2'
	auto const table_size = GetFontData(dc, os2, 0, nullptr, 0);
	if (table_size != GDI_ERROR && table_size >= 64) {
		std::array<std::uint8_t, 64> bytes{};
		if (GetFontData(
				dc, os2, 0, bytes.data(), static_cast<DWORD>(bytes.size())) ==
				static_cast<DWORD>(bytes.size())) {
			auto const intrinsic_weight = ReadBigEndianU16(bytes, 4);
			auto const selection = ReadBigEndianU16(bytes, 62);
			if (intrinsic_weight >= 1 && intrinsic_weight <= 1000) {
				result.intrinsic_weight = intrinsic_weight;
				result.intrinsic_italic =
					(selection & 0x0001u) != 0 || (selection & 0x0200u) != 0;
				result.has_intrinsic_metadata = true;
			}
		}
	}

	result.success = have_logfont && have_name && have_metrics &&
		result.font_data_fingerprint != 0 &&
		!result.selected_family_name.empty();
	SelectObject(dc, previous);
	DeleteObject(font);
	DeleteDC(dc);
	return result;
}

int ValidateDirectGdiOracle(
	GdiFontResolver& resolver,
	std::string_view family,
	int weight,
	bool italic,
	int charset,
	int height) {
	auto const oracle = ProbeDirectGdi(
		family, weight, italic, charset, height);
	if (!oracle.success || !oracle.has_intrinsic_metadata)
		return Fail("direct CreateFontIndirectW oracle could not inspect the fixture");

	bool selected_request_matches = false;
	bool selected_metrics_match = false;
	std::uint64_t selected_fingerprint = 0;
	auto const resolved = resolver.ProbeWithSelection(
		family, weight, italic, charset, height,
		[&](GdiFontSelectionView const& selected) {
			LOGFONTW logical_font{};
			TEXTMETRICW metrics{};
			auto const current_font = GetCurrentObject(selected.dc, OBJ_FONT);
			selected_request_matches = current_font &&
				GetObjectW(current_font, static_cast<int>(sizeof(logical_font)),
					&logical_font) ==
					static_cast<int>(sizeof(logical_font)) &&
				logical_font.lfHeight == oracle.logical_font.lfHeight &&
				logical_font.lfWeight == oracle.logical_font.lfWeight &&
				logical_font.lfItalic == oracle.logical_font.lfItalic &&
				logical_font.lfCharSet == oracle.logical_font.lfCharSet &&
				logical_font.lfOutPrecision == oracle.logical_font.lfOutPrecision &&
				logical_font.lfClipPrecision == oracle.logical_font.lfClipPrecision &&
				logical_font.lfQuality == oracle.logical_font.lfQuality &&
				logical_font.lfPitchAndFamily == oracle.logical_font.lfPitchAndFamily;
			selected_metrics_match = GetTextMetricsW(selected.dc, &metrics) &&
				metrics.tmWeight == oracle.metrics.tmWeight &&
				metrics.tmItalic == oracle.metrics.tmItalic &&
				metrics.tmCharSet == oracle.metrics.tmCharSet;
			selected_fingerprint = FingerprintSelectedFont(selected.dc);
		});
	if (!resolved.success || !selected_request_matches || !selected_metrics_match ||
	    selected_fingerprint == 0 ||
	    selected_fingerprint != oracle.font_data_fingerprint ||
	    resolved.selected_family_name != oracle.selected_family_name ||
	    resolved.outcome.realized_weight != oracle.intrinsic_weight ||
	    resolved.outcome.realized_italic != oracle.intrinsic_italic)
		return Fail("GDI resolver disagrees with the direct CreateFontIndirectW oracle");
	return 0;
}

std::filesystem::path ExecutableDirectory() {
	std::vector<wchar_t> buffer(32768);
	auto const length = GetModuleFileNameW(
		nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (length == 0 || length >= buffer.size())
		return {};
	return std::filesystem::path(buffer.data(), buffer.data() + length).parent_path();
}

int RunFixtureCase(std::string const& fixture_case) {
	bool const bold_only = fixture_case == "bold-only";
	bool const regular_bold = fixture_case == "regular-bold";
	bool const italic_only = fixture_case == "italic-only";
	bool const complete_rbiz = fixture_case == "rbiz";
	if (!bold_only && !regular_bold && !italic_only && !complete_rbiz)
		return Fail("unknown fixture case: " + fixture_case);

	auto const fixture_dir = ExecutableDirectory() / "gdi-font-fixtures";
	MemoryFont regular_font;
	MemoryFont bold_font;
	MemoryFont italic_font;
	MemoryFont bold_italic_font;
	if ((regular_bold || complete_rbiz) &&
	    !regular_font.Load(fixture_dir / "regular.ttf"))
		return Fail("failed to load the Regular memory-font fixture");
	if ((bold_only || regular_bold || complete_rbiz) &&
	    !bold_font.Load(fixture_dir / "bold.ttf"))
		return Fail("failed to load the Bold memory-font fixture");
	if ((italic_only || complete_rbiz) &&
	    !italic_font.Load(fixture_dir / "italic.ttf"))
		return Fail("failed to load the Italic memory-font fixture");
	if (complete_rbiz &&
	    !bold_italic_font.Load(fixture_dir / "bold-italic.ttf"))
		return Fail("failed to load the Bold Italic memory-font fixture");
	GdiFlush();

	constexpr char family[] = "Aegisub GDI Variant Fixture";
	GdiFontResolver resolver;
	auto ordinary = resolver.Probe(family, FW_NORMAL, false, DEFAULT_CHARSET, -20);
	auto bold = resolver.Probe(family, FW_BOLD, false, DEFAULT_CHARSET, -20);
	auto italic = resolver.Probe(family, FW_NORMAL, true, DEFAULT_CHARSET, -20);
	auto bold_italic = resolver.Probe(family, FW_BOLD, true, DEFAULT_CHARSET, -20);
	if (!ordinary.success || !bold.success || !italic.success || !bold_italic.success)
		return Fail("memory-font fixture could not be selected by GDI");
	{
		auto platform = CreatePlatformFontVariantResolver();
		if (!platform || !platform->Available() ||
		    platform->Info().backend != FontVariantBackend::VsFilterGdi ||
		    platform->Info().evidence != FontSelectionEvidence::Observed ||
		    !platform->Info().capabilities.confirms_physical_entity ||
		    !platform->Info().capabilities.supports_automatic_pinning)
			return Fail("Windows platform variant resolver is unavailable");
		aegisub::ass::AssFontRequest request;
		request.family = family;
		request.effective_weight = FW_NORMAL;
		request.charset = DEFAULT_CHARSET;
		request.height = 12.4;
		auto const first_size = platform->Resolve(request);
		auto const first_direct = resolver.Probe(
			family, FW_NORMAL, false, DEFAULT_CHARSET,
			QuantizeAssHeightForGdiProbe(request.height));
		request.height = 12.6;
		auto const second_size = platform->Resolve(request);
		auto const second_direct = resolver.Probe(
			family, FW_NORMAL, false, DEFAULT_CHARSET,
			QuantizeAssHeightForGdiProbe(request.height));
		if (!SameOutcome(first_size, first_direct.outcome) ||
		    !SameOutcome(second_size, second_direct.outcome))
			return Fail("platform variant resolver did not use the shared GDI probe height");
		if (!SameOutcome(first_size, second_size) ||
		    first_size.entity_token != ordinary.outcome.entity_token ||
		    first_size.status != ordinary.outcome.status ||
		    first_size.role != ordinary.outcome.role ||
		    first_size.realized_weight != ordinary.outcome.realized_weight ||
		    first_size.realized_italic != ordinary.outcome.realized_italic)
			return Fail("scalable fixture changed face across representative GDI heights");
	}
	for (auto const height : std::array{
			0, -20,
			QuantizeAssHeightForGdiProbe(12.4),
			QuantizeAssHeightForGdiProbe(12.6)}) {
		for (auto const [weight, is_italic] : std::array{
				std::pair{FW_NORMAL, false},
				std::pair{FW_BOLD, false},
				std::pair{FW_NORMAL, true},
				std::pair{FW_BOLD, true}}) {
			if (auto const oracle_result = ValidateDirectGdiOracle(
					resolver, family, weight, is_italic, DEFAULT_CHARSET, height))
				return oracle_result;
		}
	}
	if (ordinary.outcome.status != FontVariantStatus::Canonical)
		return Fail("memory-font fixture did not produce canonical GDI outcomes");

	auto profile = resolver.BuildProfile(
		family, DEFAULT_CHARSET, QuantizeAssHeightForGdiProbe(20.0));
	if (profile.backend != FontVariantBackend::VsFilterGdi ||
	    profile.evidence != FontSelectionEvidence::Observed ||
	    !profile.automatic_pinning_reliable)
		return Fail("memory-font fixture profile was not confirmed by GDI");
	if (resolver.stats().fingerprint_read_count == 0)
		return Fail("memory-font fixture did not exercise the GDI fingerprint fallback");
	{
		GdiFontResolver gdi_only(GdiFontMetadataMode::GdiOnly);
		if (!gdi_only.available() || gdi_only.dwrite_available())
			return Fail("GDI-only resolver did not disable DirectWrite metadata");
		auto const system_profile = resolver.BuildProfile(
			family, DEFAULT_CHARSET, QuantizeAssHeightForGdiProbe(20.0));
		bool callback_is_gdi_only = false;
		auto const direct = gdi_only.ProbeWithSelection(
			family, FW_NORMAL, false, DEFAULT_CHARSET, 0,
			[&](GdiFontSelectionView const& selected) {
				callback_is_gdi_only = selected.dc && !selected.dwrite_face &&
					!selected.dwrite_bridge && selected.local_file_path.empty() &&
					selected.face_index < 0;
			});
		if (!callback_is_gdi_only || !direct.success ||
		    direct.outcome.status != ordinary.outcome.status ||
		    direct.outcome.role != ordinary.outcome.role ||
		    direct.outcome.realized_weight != ordinary.outcome.realized_weight ||
		    direct.outcome.realized_italic != ordinary.outcome.realized_italic ||
		    direct.outcome.entity_token == 0 ||
		    gdi_only.stats().fingerprint_read_count == 0)
			return Fail("GDI-only resolver did not preserve the physical fixture outcome");
		auto const gdi_only_profile = gdi_only.BuildProfile(
			family, DEFAULT_CHARSET, QuantizeAssHeightForGdiProbe(20.0));
		if (!gdi_only_profile.automatic_pinning_reliable ||
		    gdi_only_profile.backend != FontVariantBackend::VsFilterGdi ||
		    gdi_only_profile.evidence != FontSelectionEvidence::Observed)
			return Fail("GDI-only resolver did not build an authoritative profile");
		for (std::size_t index = 0; index < system_profile.outcomes.size(); ++index) {
			auto const& expected = system_profile.outcomes[index];
			auto const& actual = gdi_only_profile.outcomes[index];
			if (actual.status != expected.status || actual.role != expected.role ||
			    actual.realized_weight != expected.realized_weight ||
			    actual.realized_italic != expected.realized_italic ||
			    actual.entity_token == 0)
				return Fail("GDI-only profile disagrees with system DWrite metadata");
		}
	}
	{
		FontCollectorEventSink sink;
		GdiFontFileLister lister(sink);
		aegisub::ass::AssFontRequest request;
		request.family = family;
		request.effective_weight = FW_NORMAL;
		request.charset = DEFAULT_CHARSET;
		request.height = 20.0;
		auto collected = lister.GetFontPaths(
			request, {static_cast<std::uint32_t>('A')});
		auto const resolved_bold = ordinary.outcome.role == FontVariantRole::Bold ||
		                           ordinary.outcome.role == FontVariantRole::BoldItalic;
		if (collected.matched_weight != ordinary.outcome.realized_weight ||
		    collected.matched_bold != resolved_bold ||
		    collected.matched_italic != ordinary.outcome.realized_italic)
			return Fail("GDI collector did not preserve resolver intrinsic metrics");
	}
	if (regular_bold) {
		if (bold.outcome.status != FontVariantStatus::Canonical)
			return Fail("Regular+Bold fixture did not produce a canonical Bold outcome");
		if (ordinary.outcome.role != FontVariantRole::Regular ||
		    bold.outcome.role != FontVariantRole::Bold ||
		    ordinary.outcome.entity_token == bold.outcome.entity_token)
			return Fail("Regular+Bold fixture did not select distinct physical faces");
		if (FindImplicitVariantSelection(profile) || BuildVariantChoices(profile).size() < 2)
			return Fail("Regular+Bold fixture exposed an implicit variant pin");
	}
	else if (bold_only) {
		if (bold.outcome.status != FontVariantStatus::Canonical)
			return Fail("Bold-only fixture did not produce a canonical Bold outcome");
		if (ordinary.outcome.role != FontVariantRole::Bold ||
		    bold.outcome.role != FontVariantRole::Bold ||
		    ordinary.outcome.entity_token != bold.outcome.entity_token)
			return Fail("Bold-only fixture did not select the same native Bold face");
		auto const implicit = FindImplicitVariantSelection(profile);
		if (!implicit ||
		    implicit->role != FontVariantRole::Bold ||
		    implicit->entity_token != ordinary.outcome.entity_token)
			return Fail("Bold-only fixture did not produce a safe implicit Bold pin");
	}
	else if (italic_only) {
		if (italic.outcome.status != FontVariantStatus::Canonical ||
		    ordinary.outcome.role != FontVariantRole::Italic ||
		    italic.outcome.role != FontVariantRole::Italic ||
		    ordinary.outcome.entity_token != italic.outcome.entity_token)
			return Fail("Italic-only fixture did not select the same native Italic face");
		auto const implicit = FindImplicitVariantSelection(profile);
		if (!implicit ||
		    implicit->role != FontVariantRole::Italic ||
		    implicit->entity_token != ordinary.outcome.entity_token)
			return Fail("Italic-only fixture did not produce a safe implicit Italic pin");
	}
	else {
		std::array<FontVariantOutcome const*, 4> outcomes{
			&ordinary.outcome, &bold.outcome, &italic.outcome, &bold_italic.outcome};
		std::array<FontVariantRole, 4> roles{
			FontVariantRole::Regular, FontVariantRole::Bold,
			FontVariantRole::Italic, FontVariantRole::BoldItalic};
		std::unordered_set<std::uint64_t> entities;
		for (std::size_t index = 0; index < outcomes.size(); ++index) {
			if (outcomes[index]->status != FontVariantStatus::Canonical ||
			    outcomes[index]->role != roles[index] ||
			    !entities.insert(outcomes[index]->entity_token).second)
				return Fail("complete RBIZ fixture did not select four native faces");
		}
		if (FindImplicitVariantSelection(profile) || BuildVariantChoices(profile).size() != 4)
			return Fail("complete RBIZ fixture exposed an invalid profile");
	}
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	if (auto const height_result = ValidateHeightQuantization())
		return height_result;
	if (argc == 3 && std::string_view(argv[1]) == "--fixture-case")
		return RunFixtureCase(argv[2]);
	if (argc != 1)
		return Fail("usage: gdi-font-resolver-smoke [--fixture-case bold-only|regular-bold|italic-only|rbiz]");

	auto const process_baseline = GdiObjectCount();
	{
		GdiFontResolver resolver;
		if (!resolver.available()) {
			std::cout << "gdi-font-resolver-smoke: GDI resolver unavailable; skipped\n";
			return 0;
		}

		auto const families = resolver.EnumerateFamilies();
		if (families.empty()) {
			std::cout << "gdi-font-resolver-smoke: no GDI font families; skipped\n";
			return 0;
		}
		for (std::size_t index = 0; index < families.size(); ++index) {
			auto wide = agi::charset::ConvertW(families[index]);
			if (wide.empty())
				return Fail("GDI family enumeration returned an invalid UTF-8 name");
			for (std::size_t prior_index = 0; prior_index < index; ++prior_index) {
				auto prior = agi::charset::ConvertW(families[prior_index]);
				if (CompareStringOrdinal(
						wide.data(), static_cast<int>(wide.size()),
						prior.data(), static_cast<int>(prior.size()), TRUE) == CSTR_EQUAL)
					return Fail("GDI family enumeration returned a duplicate Unicode alias");
			}
		}

		std::string family;
		GdiFontProbeResult ordinary;
		for (auto const& candidate : families) {
			auto probe = resolver.Probe(candidate, FW_NORMAL, false, DEFAULT_CHARSET, 0);
			if (probe.success && probe.outcome.entity_token != 0) {
				family = candidate;
				ordinary = std::move(probe);
				break;
			}
		}
		if (family.empty()) {
			std::cout << "gdi-font-resolver-smoke: no resolvable GDI family; skipped\n";
			return 0;
		}

		// Collection access must observe the same selected face as the probe and
		// must not trigger a second physical GDI selection.
		auto const selection_start_stats = resolver.stats();
		int callback_count = 0;
		bool callback_matches_result = false;
		auto selected_probe = resolver.ProbeWithSelection(
			family, FW_NORMAL, false, DEFAULT_CHARSET, -19,
			[&](GdiFontSelectionView const& selected) {
				++callback_count;
				wchar_t face_name[LF_FACESIZE] = {};
				callback_matches_result = selected.dc && selected.probe &&
					selected.probe->success && selected.probe->matches_requested_family &&
					GetTextFaceW(selected.dc, LF_FACESIZE, face_name) > 0 &&
					agi::charset::ConvertW(face_name) == selected.probe->selected_family_name;
			});
		auto const selection_end_stats = resolver.stats();
		if (callback_count != 1 || !callback_matches_result ||
		    !selected_probe.success || !selected_probe.matches_requested_family)
			return Fail("selected-face callback did not observe the probed GDI entity");
		if (selection_end_stats.physical_probe_count !=
				selection_start_stats.physical_probe_count + 1 ||
		    selection_end_stats.request_count != selection_start_stats.request_count + 1)
			return Fail("selected-face callback performed more than one physical probe");

		// The same resolver request must hit the memoized physical outcome. This
		// also verifies that the public token is stable without exposing a path.
		auto const repeated_start_stats = resolver.stats();
		for (int i = 0; i < 256; ++i) {
			auto repeated = resolver.Probe(family, FW_NORMAL, false, DEFAULT_CHARSET, 0);
			if (!repeated.success || !SameOutcome(ordinary.outcome, repeated.outcome))
				return Fail("repeated probe changed its physical outcome");
		}
		auto const repeated_end_stats = resolver.stats();
		if (repeated_end_stats.physical_probe_count !=
		    repeated_start_stats.physical_probe_count ||
		    repeated_end_stats.memo_hit_count - repeated_start_stats.memo_hit_count != 256 ||
		    repeated_end_stats.fingerprint_read_count !=
		    repeated_start_stats.fingerprint_read_count)
			return Fail("repeated probe bypassed the resolver memo");
		auto negative_charset = resolver.Probe(family, FW_NORMAL, false, -1, 0);
		if (!negative_charset.success ||
		    !SameOutcome(ordinary.outcome, negative_charset.outcome))
			return Fail("negative charset did not use DEFAULT_CHARSET semantics");
		for (int requested_weight : {600, 800, 900}) {
			auto numeric = resolver.Probe(
				family, requested_weight, false, DEFAULT_CHARSET, 0);
			if (numeric.outcome.status == FontVariantStatus::Canonical ||
			    numeric.outcome.role != FontVariantRole::Unknown)
				return Fail("numeric GDI request was collapsed to an RBIZ choice");
		}

		auto const before_uncached_probes = GdiObjectCount();
		auto profile = resolver.BuildProfile(family, DEFAULT_CHARSET);
		if (profile.backend != FontVariantBackend::VsFilterGdi ||
		    profile.evidence != FontSelectionEvidence::Observed)
			return Fail("GDI profile did not retain its selection authority");
		auto substituted_profile = resolver.BuildProfile(
			"Aegisub Missing Font Variant Probe", DEFAULT_CHARSET);
		if (substituted_profile.evidence != FontSelectionEvidence::Observed ||
		    substituted_profile.automatic_pinning_reliable)
			return Fail("a substituted GDI family was marked safe for automatic pinning");
		std::array<int, 4> const expected_weights{FW_NORMAL, FW_BOLD, FW_NORMAL, FW_BOLD};
		std::array<bool, 4> const expected_italics{false, false, true, true};
		for (std::size_t index = 0; index < profile.outcomes.size(); ++index) {
			auto const& outcome = profile.outcomes[index];
			if (outcome.requested_weight != expected_weights[index] ||
			    outcome.requested_italic != expected_italics[index])
				return Fail("four-way profile request order is invalid");
		}
		if (&profile.For(false, false) != &profile.outcomes[0] ||
		    &profile.For(true, false) != &profile.outcomes[1] ||
		    &profile.For(false, true) != &profile.outcomes[2] ||
		    &profile.For(true, true) != &profile.outcomes[3])
			return Fail("profile request lookup does not match RBIZ order");

		std::unordered_set<std::uint64_t> choice_entities;
		for (auto const& choice : BuildVariantChoices(profile)) {
			if (choice.status != FontVariantStatus::Canonical || choice.entity_token == 0)
				return Fail("unsafe profile outcome became a selectable choice");
			if (!choice_entities.insert(choice.entity_token).second)
				return Fail("profile contains duplicate physical choices");
		}

		// Exercise uncached combinations repeatedly. SelectedFont must restore
		// the original HDC object before deleting each temporary HFONT.
		for (int iteration = 0; iteration < 32; ++iteration) {
			for (std::size_t index = 0; index < expected_weights.size(); ++index) {
				auto probe = resolver.Probe(
					family, expected_weights[index], expected_italics[index],
					DEFAULT_CHARSET, iteration + 1);
				if (probe.outcome.requested_weight != expected_weights[index] ||
				    probe.outcome.requested_italic != expected_italics[index])
					return Fail("probe lost its requested GDI variant");
			}
		}
		auto const after_uncached_probes = GdiObjectCount();
		if (before_uncached_probes != 0 && after_uncached_probes > before_uncached_probes + 2)
			return Fail("temporary GDI font objects were not released");

		auto const height_start_stats = resolver.stats();
		auto const height_101 = resolver.Probe(
			family, FW_NORMAL, false, DEFAULT_CHARSET, 101);
		auto const height_102 = resolver.Probe(
			family, FW_NORMAL, false, DEFAULT_CHARSET, 102);
		auto const height_101_again = resolver.Probe(
			family, FW_NORMAL, false, DEFAULT_CHARSET, 101);
		auto const height_end_stats = resolver.stats();
		if (!height_101.success || !height_102.success ||
		    !SameOutcome(height_101.outcome, height_101_again.outcome) ||
		    height_end_stats.physical_probe_count !=
			    height_start_stats.physical_probe_count + 2 ||
		    height_end_stats.memo_hit_count !=
			    height_start_stats.memo_hit_count + 1)
			return Fail("low-level GDI memo did not preserve explicit LOGFONT height");

		// The platform collector path must forward the resolver's result rather
		// than leaving the new diagnostics empty.
		{
			FontCollectorEventSink sink;
			GdiFontFileLister lister(sink);
			aegisub::ass::AssFontRequest request;
			request.family = family;
			request.effective_weight = FW_NORMAL;
			request.charset = DEFAULT_CHARSET;
			request.height = 18.0;
			auto const first_size_key = lister.GetMatchKey(request);
			request.height = 72.0;
			auto const second_size_key = lister.GetMatchKey(request);
			if (!first_size_key.height || !second_size_key.height ||
				*first_size_key.height != QuantizeAssHeightForGdiProbe(18.0) ||
				*second_size_key.height != QuantizeAssHeightForGdiProbe(72.0) ||
				!(*first_size_key.height != *second_size_key.height))
				return Fail("GDI collector did not preserve representative height in its match key");
			request.height = 18.0;
			auto collected = lister.GetFontPaths(request, {static_cast<std::uint32_t>('A')});
			if (!collected.backend_requested_weight ||
			    *collected.backend_requested_weight != FW_NORMAL ||
			    !collected.realized_status)
				return Fail("GDI collector did not propagate resolver diagnostics");
			if ((*collected.realized_status == FontVariantStatus::Canonical) !=
			    !collected.noncanonical_variant)
				return Fail("GDI collector variant status flags disagree");

			request.effective_weight = 600;
			auto numeric = lister.GetFontPaths(request, {static_cast<std::uint32_t>('A')});
			if (!numeric.backend_requested_weight ||
			    *numeric.backend_requested_weight != 600 ||
			    !numeric.realized_status ||
			    *numeric.realized_status == FontVariantStatus::Canonical ||
			    !numeric.noncanonical_variant || numeric.realized_role ||
			    numeric.matched_bold)
				return Fail("GDI collector collapsed a numeric request to RBIZ");

			request.family = "Aegisub Missing Face Probe";
			auto missing_family = lister.GetFontPaths(
				request, {static_cast<std::uint32_t>('A')});
			if (!missing_family.paths.empty() || !missing_family.memory_fonts.empty() ||
			    !missing_family.realized_status ||
			    *missing_family.realized_status == FontVariantStatus::Canonical)
				return Fail("GDI collector returned the substituted default font");
		}
	}

	if (auto const catalog_result = ValidateInstalledCatalog())
		return catalog_result;

	// Resolver owns one compatible HDC. Destruction must return that object to
	// the process, allowing a small tolerance for lazy system font caches.
	auto const process_after = GdiObjectCount();
	if (process_baseline != 0 && process_after > process_baseline + 2)
		return Fail("resolver destruction leaked GDI objects");
	return 0;
}
