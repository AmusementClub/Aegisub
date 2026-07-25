// A/B equivalence tool: historical BuildFontFamilyCatalog (legacy snapshot)
// vs ObserveWindowsFontFamilies + DeriveWindowsFontFamilyCatalog.
//
// Compares catalog structure without absolute entity_token values. Writes a
// structured report under the output directory (default: font-family-ab/).
// Exit 0 when catalogs match; non-zero on differences or hard failures.
//
// Not a CI regression test: local font sets change. Record provider/OS
// fingerprints in the report for reproducibility.

#include "font_family_catalog.h"
#include "font_family_derive_win.h"
#include "font_family_obs_repository_win.h"
#include "font_family_obs_store.h"
#include "font_family_observe_win.h"
#include "font_variant_policy.h"
#include "gdi_font_resolver.h"
#include "legacy/font_family_catalog_win_legacy.h"

#include <libaegisub/charset_conv_win.h>
#include <libaegisub/log.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

char const* RoleName(FontVariantRole role) {
	switch (role) {
		case FontVariantRole::Unknown: return "Unknown";
		case FontVariantRole::Regular: return "Regular";
		case FontVariantRole::Bold: return "Bold";
		case FontVariantRole::Italic: return "Italic";
		case FontVariantRole::BoldItalic: return "BoldItalic";
	}
	return "?";
}

char const* StatusName(FontVariantStatus status) {
	switch (status) {
		case FontVariantStatus::Unknown: return "Unknown";
		case FontVariantStatus::Canonical: return "Canonical";
		case FontVariantStatus::NonCanonical: return "NonCanonical";
		case FontVariantStatus::Synthetic: return "Synthetic";
	}
	return "?";
}

char const* NameKindName(FontFamilyNameKind kind) {
	switch (kind) {
		case FontFamilyNameKind::Win32Family: return "Win32Family";
		case FontFamilyNameKind::TypographicFamily: return "TypographicFamily";
		case FontFamilyNameKind::FullName: return "FullName";
		case FontFamilyNameKind::PostScript: return "PostScript";
		case FontFamilyNameKind::PlatformAlias: return "PlatformAlias";
	}
	return "?";
}

std::string AsciiLower(std::string_view text) {
	std::string result;
	result.reserve(text.size());
	for (unsigned char ch : text)
		result.push_back(static_cast<char>(std::tolower(ch)));
	return result;
}

std::optional<std::wstring> ToUtf16(std::string_view value) {
	if (value.empty())
		return std::wstring{};
	try {
		return agi::charset::ConvertW(std::string(value));
	}
	catch (...) {
		return std::nullopt;
	}
}

bool OrdinalIcaseEqual(std::string_view left, std::string_view right) {
	auto const left_wide = ToUtf16(left);
	auto const right_wide = ToUtf16(right);
	return left_wide && right_wide &&
		CompareStringOrdinal(
			left_wide->data(), static_cast<int>(left_wide->size()),
			right_wide->data(), static_cast<int>(right_wide->size()), TRUE) == CSTR_EQUAL;
}

bool OrdinalIcaseLess(std::string const& left, std::string const& right) {
	auto const left_wide = ToUtf16(left);
	auto const right_wide = ToUtf16(right);
	if (!left_wide || !right_wide)
		return left < right;
	return CompareStringOrdinal(
		left_wide->data(), static_cast<int>(left_wide->size()),
		right_wide->data(), static_cast<int>(right_wide->size()), TRUE) == CSTR_LESS_THAN;
}

std::string UserDefaultLocale() {
	wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
	if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) <= 0)
		return {};
	try {
		return agi::charset::ConvertW(locale);
	}
	catch (...) {
		return {};
	}
}

std::string NormalizedLocale(std::string_view locale) {
	auto result = AsciiLower(locale);
	std::replace(result.begin(), result.end(), '_', '-');
	return result;
}

bool ProbeMatchesLive(
	FontFamilyProbeObservation const& recorded,
	GdiFontProbeResult const& live) {
	auto const& left = recorded.outcome;
	auto const& right = live.outcome;
	return recorded.success == live.success &&
		recorded.matches_requested_family == live.matches_requested_family &&
		left.requested_weight == right.requested_weight &&
		left.requested_italic == right.requested_italic &&
		left.realized_weight == right.realized_weight &&
		left.realized_italic == right.realized_italic &&
		left.role == right.role && left.status == right.status &&
		left.entity_token == right.entity_token;
}

struct AliasValidationResult {
	std::size_t candidate_count = 0;
	std::size_t over_gdi_limit = 0;
	std::size_t probe_count = 0;
	std::size_t mismatching_candidates = 0;
	std::size_t mismatching_probes = 0;
	std::vector<std::string> details;

	std::size_t eligible_candidates() const noexcept {
		return candidate_count - over_gdi_limit;
	}
};

AliasValidationResult ValidateAliasObservations(
	FontFamilyCatalogObservations const& observations) {
	AliasValidationResult result;
	result.candidate_count = observations.aliases.size();
	GdiFontResolver resolver;
	for (auto const& alias : observations.aliases) {
		// Derive rejects these before consulting the recorded observation, so a
		// truncated live LOGFONT request is not a meaningful equivalence check.
		if (FontFamilyCatalog::ExceedsGdiFaceNameLimit(alias.candidate_name)) {
			++result.over_gdi_limit;
			continue;
		}
		bool candidate_mismatch = false;
		for (std::size_t index = 0; index < alias.rbiz.size(); ++index) {
			bool const italic = index >= 2;
			bool const bold = (index & 1u) != 0;
			auto const live = resolver.Probe(
				alias.candidate_name, bold ? FW_BOLD : FW_NORMAL, italic,
				DEFAULT_CHARSET, 0);
			++result.probe_count;
			if (ProbeMatchesLive(alias.rbiz[index], live))
				continue;
			candidate_mismatch = true;
			++result.mismatching_probes;
			std::ostringstream detail;
			detail << alias.candidate_name << " rbiz=" << index
			       << " recorded(success=" << alias.rbiz[index].success
			       << ",match=" << alias.rbiz[index].matches_requested_family
			       << ",token=" << alias.rbiz[index].outcome.entity_token
			       << ",status=" << StatusName(alias.rbiz[index].outcome.status)
			       << ") live(success=" << live.success
			       << ",match=" << live.matches_requested_family
			       << ",token=" << live.outcome.entity_token
			       << ",status=" << StatusName(live.outcome.status) << ')';
			result.details.push_back(detail.str());
		}
		if (candidate_mismatch)
			++result.mismatching_candidates;
	}
	return result;
}

// Relative token partition: non-zero absolute tokens mapped to dense 1..N
// within one record; zeros stay zero. Absolute values are never compared.
std::array<std::uint32_t, 4> OutcomeTokenPartitions(
	FontFamilyVariantProfile const& profile) {
	std::unordered_map<std::uint64_t, std::uint32_t> map;
	std::array<std::uint32_t, 4> parts{};
	std::uint32_t next = 1;
	for (std::size_t i = 0; i < profile.outcomes.size(); ++i) {
		auto const token = profile.outcomes[i].entity_token;
		if (token == 0) {
			parts[i] = 0;
			continue;
		}
		auto [it, inserted] = map.emplace(token, next);
		if (inserted)
			++next;
		parts[i] = it->second;
	}
	return parts;
}

struct NameSig {
	std::string value;
	std::string locale;
	FontFamilyNameKind kind = FontFamilyNameKind::Win32Family;
	FontVariantRole variant_role = FontVariantRole::Unknown;
	std::uint32_t token_part = 0;

	bool operator<(NameSig const& o) const {
		if (kind != o.kind)
			return static_cast<int>(kind) < static_cast<int>(o.kind);
		if (value != o.value)
			return value < o.value;
		if (locale != o.locale)
			return locale < o.locale;
		if (variant_role != o.variant_role)
			return static_cast<int>(variant_role) < static_cast<int>(o.variant_role);
		return token_part < o.token_part;
	}

	bool operator==(NameSig const& o) const = default;
};

struct ChoiceSig {
	FontVariantRole role = FontVariantRole::Unknown;
	int weight = 400;
	bool italic = false;
	FontVariantStatus status = FontVariantStatus::Unknown;
	std::uint32_t token_part = 0;

	bool operator==(ChoiceSig const& o) const = default;
};

struct OutcomeSig {
	int requested_weight = 400;
	bool requested_italic = false;
	int realized_weight = 0;
	bool realized_italic = false;
	FontVariantRole role = FontVariantRole::Unknown;
	FontVariantStatus status = FontVariantStatus::Unknown;
	std::uint32_t token_part = 0;

	bool operator==(OutcomeSig const& o) const = default;
};

struct FamilySig {
	std::string localized;
	std::string english;
	std::vector<NameSig> names;
	std::array<OutcomeSig, 4> outcomes{};
	bool automatic_pinning_reliable = false;
	std::vector<ChoiceSig> choices;
	std::optional<ChoiceSig> implicit_pin;
	// Stable match key: sorted Win32 family name values (ordinal-icase).
	std::vector<std::string> win32_keys;

	std::string MatchKey() const {
		std::ostringstream oss;
		for (std::size_t i = 0; i < win32_keys.size(); ++i) {
			if (i)
				oss << '\x1f';
			oss << win32_keys[i];
		}
		if (win32_keys.empty())
			oss << localized;
		return oss.str();
	}
};

FamilySig BuildFamilySig(FontFamilyRecord const& record) {
	FamilySig sig;
	sig.localized = record.localized_family_name;
	sig.english = record.english_win32_family_name;
	sig.automatic_pinning_reliable = record.variant_profile.automatic_pinning_reliable;
	auto const parts = OutcomeTokenPartitions(record.variant_profile);

	// Map absolute name tokens -> relative partitions within this record.
	std::unordered_map<std::uint64_t, std::uint32_t> name_token_map;
	std::uint32_t next_external = 0x10000u;
	auto map_token = [&](std::uint64_t token) -> std::uint32_t {
		if (token == 0)
			return 0;
		for (std::size_t i = 0; i < record.variant_profile.outcomes.size(); ++i)
			if (record.variant_profile.outcomes[i].entity_token == token)
				return parts[i];
		auto [it, inserted] = name_token_map.emplace(token, next_external);
		if (inserted)
			++next_external;
		return it->second;
	};

	for (std::size_t i = 0; i < 4; ++i) {
		auto const& o = record.variant_profile.outcomes[i];
		sig.outcomes[i] = OutcomeSig{
			o.requested_weight, o.requested_italic, o.realized_weight, o.realized_italic,
			o.role, o.status, parts[i]};
	}

	sig.names.reserve(record.names.size());
	for (auto const& name : record.names) {
		sig.names.push_back(NameSig{
			name.value, name.locale, name.kind, name.variant_role,
			map_token(name.entity_token)});
		if (name.kind == FontFamilyNameKind::Win32Family)
			sig.win32_keys.push_back(name.value);
	}
	std::sort(sig.names.begin(), sig.names.end());
	std::sort(sig.win32_keys.begin(), sig.win32_keys.end(), OrdinalIcaseLess);
	sig.win32_keys.erase(
		std::unique(sig.win32_keys.begin(), sig.win32_keys.end(), OrdinalIcaseEqual),
		sig.win32_keys.end());

	for (auto const& choice : BuildVariantChoices(record.variant_profile)) {
		sig.choices.push_back(ChoiceSig{
			choice.role, choice.weight, choice.italic, choice.status,
			map_token(choice.entity_token)});
	}
	if (auto pin = FindImplicitVariantSelection(record.variant_profile)) {
		sig.implicit_pin = ChoiceSig{
			pin->role, pin->weight, pin->italic, pin->status,
			map_token(pin->entity_token)};
	}
	return sig;
}

std::string FormatOutcome(OutcomeSig const& o) {
	std::ostringstream oss;
	oss << "req=" << o.requested_weight << (o.requested_italic ? "I" : "")
	    << " realized=" << o.realized_weight << (o.realized_italic ? "I" : "")
	    << " role=" << RoleName(o.role) << " status=" << StatusName(o.status)
	    << " token_part=" << o.token_part;
	return oss.str();
}

std::string FormatChoice(ChoiceSig const& c) {
	std::ostringstream oss;
	oss << RoleName(c.role) << " w=" << c.weight << (c.italic ? " I" : "")
	    << " " << StatusName(c.status) << " part=" << c.token_part;
	return oss.str();
}

void DiffFamily(
	std::ostream& out,
	std::string const& key,
	FamilySig const* legacy,
	FamilySig const* modern) {
	out << "--- family key: " << key << " ---\n";
	if (!legacy) {
		out << "  only in modern (Observe+Derive)\n";
		return;
	}
	if (!modern) {
		out << "  only in legacy\n";
		return;
	}
	auto field = [&](char const* name, std::string const& a, std::string const& b) {
		if (a != b)
			out << "  " << name << ": legacy=\"" << a << "\" modern=\"" << b << "\"\n";
	};
	field("localized", legacy->localized, modern->localized);
	field("english", legacy->english, modern->english);
	if (legacy->automatic_pinning_reliable != modern->automatic_pinning_reliable)
		out << "  automatic_pinning_reliable: legacy="
		    << legacy->automatic_pinning_reliable
		    << " modern=" << modern->automatic_pinning_reliable << '\n';
	for (std::size_t i = 0; i < 4; ++i) {
		if (legacy->outcomes[i] != modern->outcomes[i]) {
			out << "  outcome[" << i << "]:\n"
			    << "    legacy: " << FormatOutcome(legacy->outcomes[i]) << '\n'
			    << "    modern: " << FormatOutcome(modern->outcomes[i]) << '\n';
		}
	}
	if (legacy->names != modern->names) {
		out << "  names mismatch (legacy " << legacy->names.size()
		    << " vs modern " << modern->names.size() << ")\n";
		// List symmetric difference of string forms.
		std::vector<std::string> left;
		std::vector<std::string> right;
		auto fmt = [](NameSig const& n) {
			std::ostringstream oss;
			oss << NameKindName(n.kind) << '|' << n.value << '|' << n.locale
			    << '|' << RoleName(n.variant_role) << "|part=" << n.token_part;
			return oss.str();
		};
		for (auto const& n : legacy->names)
			left.push_back(fmt(n));
		for (auto const& n : modern->names)
			right.push_back(fmt(n));
		std::sort(left.begin(), left.end());
		std::sort(right.begin(), right.end());
		std::vector<std::string> only_l;
		std::vector<std::string> only_r;
		std::set_difference(left.begin(), left.end(), right.begin(), right.end(),
			std::back_inserter(only_l));
		std::set_difference(right.begin(), right.end(), left.begin(), left.end(),
			std::back_inserter(only_r));
		for (auto const& s : only_l)
			out << "    only legacy: " << s << '\n';
		for (auto const& s : only_r)
			out << "    only modern: " << s << '\n';
	}
	if (legacy->choices != modern->choices) {
		out << "  variant choices mismatch\n";
		out << "    legacy:";
		for (auto const& c : legacy->choices)
			out << " [" << FormatChoice(c) << ']';
		out << "\n    modern:";
		for (auto const& c : modern->choices)
			out << " [" << FormatChoice(c) << ']';
		out << '\n';
	}
	if (legacy->implicit_pin != modern->implicit_pin) {
		out << "  implicit pin: legacy=";
		if (legacy->implicit_pin)
			out << FormatChoice(*legacy->implicit_pin);
		else
			out << "(none)";
		out << " modern=";
		if (modern->implicit_pin)
			out << FormatChoice(*modern->implicit_pin);
		else
			out << "(none)";
		out << '\n';
	}
}

bool FamiliesEqual(FamilySig const& a, FamilySig const& b) {
	return a.localized == b.localized && a.english == b.english &&
		a.names == b.names && a.outcomes == b.outcomes &&
		a.automatic_pinning_reliable == b.automatic_pinning_reliable &&
		a.choices == b.choices && a.implicit_pin == b.implicit_pin;
}

struct CompareResult {
	std::size_t legacy_count = 0;
	std::size_t modern_count = 0;
	std::size_t matched = 0;
	std::size_t differing = 0;
	std::size_t only_legacy = 0;
	std::size_t only_modern = 0;
	std::vector<std::string> diff_lines;
};

CompareResult CompareCatalogs(
	FontFamilyCatalog const& legacy,
	FontFamilyCatalog const& modern) {
	CompareResult result;
	result.legacy_count = legacy.size();
	result.modern_count = modern.size();

	// Multimap: same Win32 key set can theoretically collide; keep lists.
	std::map<std::string, std::vector<FamilySig>> legacy_by_key;
	std::map<std::string, std::vector<FamilySig>> modern_by_key;
	for (auto const& record : legacy.records()) {
		auto sig = BuildFamilySig(record);
		legacy_by_key[sig.MatchKey()].push_back(std::move(sig));
	}
	for (auto const& record : modern.records()) {
		auto sig = BuildFamilySig(record);
		modern_by_key[sig.MatchKey()].push_back(std::move(sig));
	}

	std::ostringstream diff;
	auto all_keys = [&] {
		std::vector<std::string> keys;
		for (auto const& [k, _] : legacy_by_key)
			keys.push_back(k);
		for (auto const& [k, _] : modern_by_key)
			if (legacy_by_key.find(k) == legacy_by_key.end())
				keys.push_back(k);
		std::sort(keys.begin(), keys.end());
		return keys;
	}();

	for (auto const& key : all_keys) {
		auto lit = legacy_by_key.find(key);
		auto mit = modern_by_key.find(key);
		std::vector<FamilySig> empty;
		auto& lvec = lit != legacy_by_key.end() ? lit->second : empty;
		auto& mvec = mit != modern_by_key.end() ? mit->second : empty;

		// Greedy match equal pairs first, then pair remaining by index.
		std::vector<bool> l_used(lvec.size(), false);
		std::vector<bool> m_used(mvec.size(), false);
		for (std::size_t i = 0; i < lvec.size(); ++i) {
			for (std::size_t j = 0; j < mvec.size(); ++j) {
				if (m_used[j])
					continue;
				if (FamiliesEqual(lvec[i], mvec[j])) {
					l_used[i] = m_used[j] = true;
					++result.matched;
					break;
				}
			}
		}
		for (std::size_t i = 0; i < lvec.size(); ++i) {
			if (l_used[i])
				continue;
			std::optional<std::size_t> pair;
			for (std::size_t j = 0; j < mvec.size(); ++j) {
				if (!m_used[j]) {
					pair = j;
					break;
				}
			}
			if (pair) {
				m_used[*pair] = true;
				++result.differing;
				DiffFamily(diff, key, &lvec[i], &mvec[*pair]);
			}
			else {
				++result.only_legacy;
				DiffFamily(diff, key, &lvec[i], nullptr);
			}
		}
		for (std::size_t j = 0; j < mvec.size(); ++j) {
			if (m_used[j])
				continue;
			++result.only_modern;
			DiffFamily(diff, key, nullptr, &mvec[j]);
		}
	}

	auto text = diff.str();
	if (!text.empty()) {
		std::istringstream in(text);
		std::string line;
		while (std::getline(in, line))
			result.diff_lines.push_back(std::move(line));
	}
	return result;
}

void PrintUsage(char const* argv0) {
	std::cerr
		<< "Usage: " << argv0 << " [--out-dir DIR]\n"
		<< "  Build legacy catalog and Observe+Derive catalog, compare structure.\n"
		<< "  Writes report under DIR (default: font-family-ab/).\n"
		<< "  Exit 0 on match, 1 on differences, 2 on hard failure.\n";
}

} // namespace

int main(int argc, char** argv) {
	fs::path out_dir = "font-family-ab";
	for (int i = 1; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "--help" || arg == "-h") {
			PrintUsage(argv[0]);
			return 0;
		}
		if (arg == "--out-dir") {
			if (i + 1 >= argc) {
				std::cerr << "font-family-ab-compare: --out-dir requires a path\n";
				return 2;
			}
			out_dir = argv[++i];
			continue;
		}
		std::cerr << "font-family-ab-compare: unknown argument: " << arg << '\n';
		PrintUsage(argv[0]);
		return 2;
	}

	std::error_code ec;
	fs::create_directories(out_dir, ec);
	if (ec) {
		std::cerr << "font-family-ab-compare: cannot create out-dir: "
		          << out_dir.string() << " (" << ec.message() << ")\n";
		return 2;
	}

	// Minimal logging so legacy/modern builder LOG_* lines are visible.
	agi::log::log = new agi::log::LogSink;
	agi::log::log->Subscribe(std::make_unique<agi::log::JsonEmitter>(out_dir.string()));

	auto const locale = NormalizedLocale(UserDefaultLocale());
	std::cout << "font-family-ab-compare: locale=" << locale << '\n';
	std::cout << "font-family-ab-compare: building legacy catalog...\n";
	auto const legacy_started = std::chrono::steady_clock::now();
	auto const legacy = BuildFontFamilyCatalogLegacy();
	auto const legacy_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - legacy_started).count();
	std::cout << "font-family-ab-compare: legacy families=" << legacy.size()
	          << " in " << legacy_ms << " ms\n";

	std::cout << "font-family-ab-compare: Observe + Derive...\n";
	auto const modern_started = std::chrono::steady_clock::now();
	GdiFontResolver resolver;
	auto observe = ObserveWindowsFontFamilies(resolver, [] { return false; });
	auto& observations = observe.observations;
	if (!observations.complete) {
		std::cerr << "font-family-ab-compare: observation incomplete\n";
		return 2;
	}
	auto const modern = DeriveWindowsFontFamilyCatalog(observations, locale);
	auto const modern_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - modern_started).count();
	// Parallel Observe uses per-worker resolvers; physical_probe_count is the
	// authoritative total for this snapshot.
	auto const modern_probes = observe.physical_probe_count
		? observe.physical_probe_count
		: resolver.stats().physical_probe_count;
	std::cout << "font-family-ab-compare: modern families=" << modern.size()
	          << " seeds=" << observations.seeds.size()
	          << " aliases=" << observations.aliases.size()
	          << " faces=" << observations.faces.size()
	          << " physical_probes=" << modern_probes
	          << " in " << modern_ms << " ms\n";

	std::cout << "font-family-ab-compare: validating recorded aliases with live GDI...\n";
	auto const alias_validation = ValidateAliasObservations(observations);
	std::cout << "font-family-ab-compare: alias_validation candidates="
	          << alias_validation.candidate_count
	          << " over_gdi_limit=" << alias_validation.over_gdi_limit
	          << " eligible=" << alias_validation.eligible_candidates()
	          << " probes=" << alias_validation.probe_count
	          << " mismatching_candidates="
	          << alias_validation.mismatching_candidates
	          << " mismatching_probes=" << alias_validation.mismatching_probes
	          << '\n';

	// Fingerprints for reproducibility notes (enumeration only; no extra probes).
	GdiFontResolver manifest_resolver;
	auto const manifest = CollectWindowsFontFamilyManifest(manifest_resolver);

	// Phase F profile: encode + atomic save cost on a real observation snapshot.
	// Plan threshold: <50ms save on ~570 families → leave StringPool as-is.
	FontFamilyObsStorePayload store_payload;
	store_payload.manifest = manifest;
	store_payload.observations = observations;
	store_payload.observation_contract_version =
		kFontFamilyObsObservationContractVersion;
	store_payload.derivation_contract_version =
		kFontFamilyObsDerivationContractVersion;
	store_payload.created_utc_unix = 0;
	std::string encoded;
	auto const encode_started = std::chrono::steady_clock::now();
	auto const encode_status = EncodeFontFamilyObsStore(store_payload, encoded);
	auto const encode_us = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - encode_started).count();
	std::int64_t save_us = -1;
	FontFamilyObsStoreStatus save_status = FontFamilyObsStoreStatus::RefuseWrite;
	if (encode_status == FontFamilyObsStoreStatus::Ok) {
		// agi::fs::path accepts UTF-8 relative segments under the report dir.
		auto const cache_path = agi::fs::PathFromString(
			(out_dir / "profile-obs-cache.afco").string());
		auto const save_started = std::chrono::steady_clock::now();
		save_status = SaveFontFamilyObsStoreAtomic(cache_path, store_payload);
		save_us = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - save_started).count();
	}
	std::cout << "font-family-ab-compare: store encode="
	          << FontFamilyObsStoreStatusName(encode_status)
	          << " " << (encode_us / 1000.0) << " ms"
	          << " bytes=" << encoded.size();
	if (save_us >= 0) {
		std::cout << " save=" << FontFamilyObsStoreStatusName(save_status)
		          << " " << (save_us / 1000.0) << " ms";
	}
	std::cout << " (Phase F: leave StringPool if save < 50 ms)\n";

	auto const compare = CompareCatalogs(legacy, modern);

	fs::path const summary_path = out_dir / "summary.txt";
	fs::path const diff_path = out_dir / "diff.txt";
	fs::path const meta_path = out_dir / "meta.txt";
	fs::path const alias_path = out_dir / "alias-validation.txt";

	{
		std::ofstream meta(meta_path, std::ios::binary);
		meta << "locale=" << locale << '\n'
		     << "provider_fingerprint=" << manifest.provider_fingerprint << '\n'
		     << "os_build_fingerprint=" << manifest.os_build_fingerprint << '\n'
		     << "font_registry_fingerprint="
		     << manifest.font_registry_fingerprint << '\n'
		     << "gdi_family_names=" << manifest.gdi_family_names.size() << '\n'
		     << "legacy_families=" << compare.legacy_count << '\n'
		     << "modern_families=" << compare.modern_count << '\n'
		     << "legacy_ms=" << legacy_ms << '\n'
		     << "modern_ms=" << modern_ms << '\n'
		     << "modern_physical_probes=" << modern_probes << '\n'
		     << "observation_faces=" << observations.faces.size() << '\n'
		     << "observation_seeds=" << observations.seeds.size() << '\n'
		     << "observation_aliases=" << observations.aliases.size() << '\n'
		     << "alias_over_gdi_limit=" << alias_validation.over_gdi_limit << '\n'
		     << "alias_eligible_candidates="
		     << alias_validation.eligible_candidates() << '\n'
		     << "alias_validation_probes=" << alias_validation.probe_count << '\n'
		     << "alias_mismatching_candidates="
		     << alias_validation.mismatching_candidates << '\n'
		     << "alias_mismatching_probes="
		     << alias_validation.mismatching_probes << '\n'
		     << "matched=" << compare.matched << '\n'
		     << "differing=" << compare.differing << '\n'
		     << "only_legacy=" << compare.only_legacy << '\n'
		     << "only_modern=" << compare.only_modern << '\n'
		     << "store_encode_status="
		     << FontFamilyObsStoreStatusName(encode_status) << '\n'
		     << "store_encode_us=" << encode_us << '\n'
		     << "store_encode_bytes=" << encoded.size() << '\n'
		     << "store_save_status="
		     << FontFamilyObsStoreStatusName(save_status) << '\n'
		     << "store_save_us=" << save_us << '\n'
		     << "phase_f_stringpool_threshold_ms=50\n"
		     << "phase_f_stringpool_action="
		     << (save_us >= 0 && save_us < 50'000 ? "close_no_change" : "consider_optimize")
		     << '\n';
	}

	{
		std::ofstream summary(summary_path, std::ios::binary);
		bool const ok = compare.differing == 0 && compare.only_legacy == 0 &&
			compare.only_modern == 0 && compare.legacy_count == compare.modern_count &&
			alias_validation.mismatching_probes == 0;
		summary << (ok ? "MATCH\n" : "DIFF\n")
		        << "legacy_families=" << compare.legacy_count << '\n'
		        << "modern_families=" << compare.modern_count << '\n'
		        << "matched=" << compare.matched << '\n'
		        << "differing=" << compare.differing << '\n'
		        << "only_legacy=" << compare.only_legacy << '\n'
		        << "only_modern=" << compare.only_modern << '\n';
	}

	{
		std::ofstream aliases(alias_path, std::ios::binary);
		aliases << "candidates=" << alias_validation.candidate_count << '\n'
		        << "over_gdi_limit=" << alias_validation.over_gdi_limit << '\n'
		        << "eligible=" << alias_validation.eligible_candidates() << '\n'
		        << "probes=" << alias_validation.probe_count << '\n'
		        << "mismatching_candidates="
		        << alias_validation.mismatching_candidates << '\n'
		        << "mismatching_probes=" << alias_validation.mismatching_probes
		        << '\n';
		if (alias_validation.details.empty())
			aliases << "(all eligible aliases match live four-way GDI selection; "
			           "over-limit aliases are rejected by Derive)\n";
		else
			for (auto const& detail : alias_validation.details)
				aliases << detail << '\n';
	}

	{
		std::ofstream diff(diff_path, std::ios::binary);
		if (compare.diff_lines.empty())
			diff << "(no structural differences; entity_token absolute values not compared)\n";
		else
			for (auto const& line : compare.diff_lines)
				diff << line << '\n';
	}

	bool const ok = compare.differing == 0 && compare.only_legacy == 0 &&
		compare.only_modern == 0 && compare.legacy_count == compare.modern_count &&
		alias_validation.mismatching_probes == 0;
	std::cout << "font-family-ab-compare: "
	          << (ok ? "MATCH" : "DIFF")
	          << " matched=" << compare.matched
	          << " differing=" << compare.differing
	          << " only_legacy=" << compare.only_legacy
	          << " only_modern=" << compare.only_modern
	          << "\n  report: " << out_dir.string() << '\n';
	return ok ? 0 : 1;
}
