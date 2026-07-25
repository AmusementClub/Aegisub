#include "font_family_derive_win.h"

#include "font_family_obs_internal_win.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using font_family_obs_internal::ascii_lower;
using font_family_obs_internal::ordinal_icase_equal;

constexpr std::size_t kGdiFaceNameMaxUnits = LF_FACESIZE - 1;

bool is_english_locale(std::string const& locale) {
	if (locale.size() < 2)
		return false;
	auto const a = static_cast<char>(std::tolower(static_cast<unsigned char>(locale[0])));
	auto const b = static_cast<char>(std::tolower(static_cast<unsigned char>(locale[1])));
	return a == 'e' && b == 'n' &&
		(locale.size() == 2 || locale[2] == '-' || locale[2] == '_');
}

bool is_en_us(std::string const& locale) {
	auto folded = ascii_lower(locale);
	return folded == "en-us" || folded == "en_us";
}

std::string normalized_locale(std::string_view locale) {
	auto result = ascii_lower(locale);
	std::replace(result.begin(), result.end(), '_', '-');
	return result;
}

std::string primary_language(std::string_view locale) {
	auto normalized = normalized_locale(locale);
	auto const separator = normalized.find('-');
	if (separator != std::string::npos)
		normalized.resize(separator);
	return normalized;
}

struct PhysicalVariantKeyHash {
	std::size_t operator()(PhysicalVariantKey const& key) const noexcept {
		return std::visit(
			[](auto const& value) -> std::size_t {
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, LiveVariantKey>) {
					return static_cast<std::size_t>(value.process_token);
				} else {
					std::size_t h = value.identity.volume_serial;
					h ^= static_cast<std::size_t>(value.identity.file_index) +
						0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
					h ^= static_cast<std::size_t>(value.identity.face_index) +
						0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
					h ^= static_cast<std::size_t>(value.identity.size) +
						0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
					h ^= static_cast<std::size_t>(value.identity.mtime_utc_100ns) +
						0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
					h ^= static_cast<std::size_t>(value.realized_weight) +
						0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
					h ^= (value.realized_italic ? 1u : 0u) +
						0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
					return h;
				}
			},
			key);
	}
};

/// Resolve the interning key for one probe. Durable when FaceIdentity is known;
/// Live when only a non-zero process token exists; empty when identity is
/// unknowable (snapshot token stays 0).
std::optional<PhysicalVariantKey> physical_key_for_probe(
	FontFamilyCatalogObservations const& observations,
	FontFamilyProbeObservation const& probe) {
	if (probe.face_ref != kFontFamilyInvalidFaceRef &&
	    probe.face_ref < observations.faces.size()) {
		auto const& face = observations.faces[probe.face_ref];
		if (face.IsKnown()) {
			return PhysicalVariantKey{DurableVariantKey{
				face, probe.outcome.realized_weight, probe.outcome.realized_italic}};
		}
	}
	if (probe.outcome.entity_token != 0)
		return PhysicalVariantKey{LiveVariantKey{probe.outcome.entity_token}};
	return std::nullopt;
}

/// Snapshot-local dense tokens [1..N] from PhysicalVariantKey. Absolute process
/// tokens and serialized diagnostic token fields are not used as identity.
class SnapshotTokenInterner {
	std::unordered_map<PhysicalVariantKey, std::uint64_t, PhysicalVariantKeyHash> map_;
	std::uint64_t next_ = 1;

public:
	std::uint64_t Intern(std::optional<PhysicalVariantKey> const& key) {
		if (!key)
			return 0;
		auto [it, inserted] = map_.emplace(*key, next_);
		if (inserted)
			++next_;
		return it->second;
	}
};

FontVariantOutcome snapshot_outcome(
	FontFamilyCatalogObservations const& observations,
	FontFamilyProbeObservation const& probe,
	SnapshotTokenInterner& interner) {
	auto outcome = probe.outcome;
	outcome.entity_token = interner.Intern(physical_key_for_probe(observations, probe));
	return outcome;
}

bool same_physical_outcome(
	FontVariantOutcome const& left,
	FontVariantOutcome const& right) noexcept {
	return left.entity_token != 0 && right.entity_token != 0 &&
		left.entity_token == right.entity_token &&
		left.requested_weight == right.requested_weight &&
		left.requested_italic == right.requested_italic &&
		left.realized_weight == right.realized_weight &&
		left.realized_italic == right.realized_italic &&
		left.role == right.role && left.status == right.status;
}

bool same_physical_profile(
	FontFamilyVariantProfile const& left,
	FontFamilyVariantProfile const& right) noexcept {
	if (left.backend != right.backend || left.evidence != right.evidence ||
		left.automatic_pinning_reliable != right.automatic_pinning_reliable)
		return false;
	for (std::size_t index = 0; index < left.outcomes.size(); ++index)
		if (!same_physical_outcome(left.outcomes[index], right.outcomes[index]))
			return false;
	return true;
}

FontFamilyAliasObservation const* find_alias_observation(
	FontFamilyCatalogObservations const& observations,
	std::string_view candidate) {
	auto const it = std::find_if(
		observations.aliases.begin(), observations.aliases.end(),
		[&](FontFamilyAliasObservation const& alias) {
			return ordinal_icase_equal(alias.candidate_name, candidate);
		});
	return it == observations.aliases.end() ? nullptr : &*it;
}

bool maps_to_profile(
	FontFamilyCatalogObservations const& observations,
	SnapshotTokenInterner& interner,
	std::string const& candidate,
	FontFamilyVariantProfile const& expected_profile) {
	if (candidate.empty() ||
		FontFamilyCatalog::Utf16CodeUnitLength(candidate) > kGdiFaceNameMaxUnits)
		return false;
	auto const* observed = find_alias_observation(observations, candidate);
	if (!observed)
		return false;
	for (std::size_t index = 0; index < expected_profile.outcomes.size(); ++index) {
		auto const actual = snapshot_outcome(observations, observed->rbiz[index], interner);
		if (!observed->rbiz[index].success ||
			!observed->rbiz[index].matches_requested_family ||
			!same_physical_outcome(actual, expected_profile.outcomes[index]))
			return false;
	}
	return true;
}

void add_name(FontFamilyRecord& record, FontFamilyName name) {
	if (name.value.empty())
		return;
	auto const duplicate = std::find_if(record.names.begin(), record.names.end(),
		[&](FontFamilyName const& current) {
			return current.value == name.value && current.locale == name.locale &&
				current.kind == name.kind && current.entity_token == name.entity_token &&
				current.variant_role == name.variant_role;
		});
	if (duplicate == record.names.end())
		record.names.push_back(std::move(name));
}

void merge_record_names(FontFamilyRecord& target, FontFamilyRecord&& source) {
	for (auto& candidate : source.names)
		add_name(target, std::move(candidate));
}

std::string pick_english_name(
	FontFamilyCatalogObservations const& observations,
	SnapshotTokenInterner& interner,
	FontFamilyRecord const& record) {
	std::vector<std::string> en_us;
	std::vector<std::string> other_english;
	for (auto const& name : record.names) {
		if (name.kind != FontFamilyNameKind::Win32Family || !is_english_locale(name.locale))
			continue;
		auto& candidates = is_en_us(name.locale) ? en_us : other_english;
		if (std::none_of(candidates.begin(), candidates.end(),
			[&](std::string const& current) {
				return ordinal_icase_equal(current, name.value);
			}))
			candidates.push_back(name.value);
	}
	for (auto const& candidate : en_us)
		if (maps_to_profile(observations, interner, candidate, record.variant_profile))
			return candidate;
	for (auto const& candidate : other_english)
		if (maps_to_profile(observations, interner, candidate, record.variant_profile))
			return candidate;
	return {};
}

std::string pick_localized_name(
	FontFamilyCatalogObservations const& observations,
	SnapshotTokenInterner& interner,
	FontFamilyRecord const& record,
	std::string_view locale) {
	if (locale.empty())
		return record.localized_family_name;
	auto const normalized = normalized_locale(locale);
	auto const language = primary_language(normalized);
	for (int rank : {2, 1}) {
		for (auto const& name : record.names) {
			if (name.kind != FontFamilyNameKind::Win32Family || name.locale.empty())
				continue;
			auto const candidate_locale = normalized_locale(name.locale);
			bool const matches = rank == 2
				? candidate_locale == normalized
				: !language.empty() && primary_language(candidate_locale) == language;
			if (matches && maps_to_profile(
					observations, interner, name.value, record.variant_profile))
				return name.value;
		}
	}
	return record.localized_family_name;
}

} // namespace

FontFamilyCatalog DeriveWindowsFontFamilyCatalog(
	FontFamilyCatalogObservations const& observations,
	std::string_view locale) {
	if (!observations.complete)
		return {};

	// One interner for the whole Derive pass so seed profiles and alias checks
	// share the same dense snapshot-local token space.
	SnapshotTokenInterner interner;

	std::vector<FontFamilyRecord> records;
	records.reserve(observations.seeds.size());
	std::unordered_map<std::uint64_t, std::vector<std::size_t>> profile_owners;

	for (auto const& seed : observations.seeds) {
		FontFamilyRecord record;
		record.localized_family_name = seed.seed_family_name;
		add_name(record, {seed.seed_family_name, {}, FontFamilyNameKind::Win32Family});
		for (auto const& name : seed.win32_family_names)
			add_name(record, name);
		std::array<FontVariantOutcome, 4> outcomes;
		for (std::size_t index = 0; index < outcomes.size(); ++index) {
			outcomes[index] = snapshot_outcome(observations, seed.rbiz[index], interner);
			// Informational names inherit the probe's snapshot token; serialized
			// process tokens are never trusted as identity. Intentional copy so
			// the remapped token does not mutate the observation input.
			for (auto name_copy : seed.rbiz[index].informational_names) {
				name_copy.entity_token = outcomes[index].entity_token;
				add_name(record, std::move(name_copy));
			}
		}
		record.variant_profile = BuildFontFamilyVariantProfile(
			std::move(outcomes), FontVariantBackend::VsFilterGdi,
			FontSelectionEvidence::Observed, seed.profile_matches_requested_family);

		bool merged = false;
		if (seed.profile_matches_requested_family) {
			auto const entity = record.variant_profile.For(false, false).entity_token;
			auto& candidates = profile_owners[entity];
			for (auto const candidate_index : candidates) {
				if (!same_physical_profile(
						records[candidate_index].variant_profile, record.variant_profile))
					continue;
				merge_record_names(records[candidate_index], std::move(record));
				merged = true;
				break;
			}
			if (!merged)
				candidates.push_back(records.size());
		}
		if (!merged)
			records.push_back(std::move(record));
	}

	std::vector<std::pair<std::string, std::vector<std::size_t>>> english_owners;
	for (std::size_t index = 0; index < records.size(); ++index) {
		auto& record = records[index];
		if (record.variant_profile.For(false, false).entity_token == 0)
			continue;
		record.localized_family_name =
			pick_localized_name(observations, interner, record, locale);
		record.english_win32_family_name =
			pick_english_name(observations, interner, record);
		if (record.english_win32_family_name.empty())
			continue;
		auto owner = std::find_if(english_owners.begin(), english_owners.end(),
			[&](auto const& entry) {
				return ordinal_icase_equal(entry.first, record.english_win32_family_name);
			});
		if (owner == english_owners.end())
			english_owners.push_back({record.english_win32_family_name, {index}});
		else
			owner->second.push_back(index);
	}

	for (auto const& [name, owners] : english_owners) {
		(void)name;
		if (owners.size() > 1)
			for (auto const index : owners)
				records[index].english_win32_family_name.clear();
	}
	// IDs are ephemeral snapshot identifiers. Allocate densely only after all
	// physical merges, so every replay produces index + 1 without preserving
	// discarded seed IDs or historical holes.
	for (FontFamilyId id = 1; auto& record : records)
		record.id = id++;
	return FontFamilyCatalog(std::move(records));
}
