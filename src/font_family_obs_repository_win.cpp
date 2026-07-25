#include "font_family_obs_repository_win.h"

#include "font_family_derive_win.h"
#include "font_family_obs_internal_win.h"
#include "font_family_observe_win.h"
#include "gdi_font_resolver.h"
#include "options.h"

// Face stamp M1 helpers live in font_family_obs_face_stamp_win (included via header).

#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using font_family_obs_internal::ordinal_icase_equal;
using font_family_obs_internal::to_utf16;

// Manifest family-name sort is repository-local; not shared with observe/derive.
bool ordinal_icase_less(std::string_view left, std::string_view right) {
	auto const left_wide = to_utf16(left);
	auto const right_wide = to_utf16(right);
	if (!left_wide || !right_wide)
		return left < right;
	return CompareStringOrdinal(
		left_wide->data(), static_cast<int>(left_wide->size()),
		right_wide->data(), static_cast<int>(right_wide->size()), TRUE) == CSTR_LESS_THAN;
}

std::uint64_t fnv1a64(std::string_view data, std::uint64_t hash = 14695981039346656037ull) {
	for (unsigned char byte : data) {
		hash ^= byte;
		hash *= 1099511628211ull;
	}
	return hash;
}

std::uint64_t fnv1a64_u32(std::uint32_t value, std::uint64_t hash = 14695981039346656037ull) {
	char bytes[4] = {
		static_cast<char>(value & 0xFF),
		static_cast<char>((value >> 8) & 0xFF),
		static_cast<char>((value >> 16) & 0xFF),
		static_cast<char>((value >> 24) & 0xFF),
	};
	return fnv1a64(std::string_view(bytes, 4), hash);
}

std::vector<std::string> sorted_unique_family_names(std::vector<std::string> names) {
	std::sort(names.begin(), names.end(),
		[](std::string const& left, std::string const& right) {
			return ordinal_icase_less(left, right);
		});
	names.erase(
		std::unique(names.begin(), names.end(),
			[](std::string const& left, std::string const& right) {
				return ordinal_icase_equal(left, right);
			}),
		names.end());
	return names;
}

std::vector<std::string> family_names_from_seeds(
	FontFamilyCatalogObservations const& observations) {
	std::vector<std::string> names;
	names.reserve(observations.seeds.size());
	for (auto const& seed : observations.seeds)
		names.push_back(seed.seed_family_name);
	return sorted_unique_family_names(std::move(names));
}

std::uint64_t os_build_fingerprint() {
	std::uint64_t hash = 14695981039346656037ull;
	HKEY key = nullptr;
	if (RegOpenKeyExW(
			HKEY_LOCAL_MACHINE,
			L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
			KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
		return 0;
	auto read_string = [&](wchar_t const* name) {
		wchar_t buffer[256];
		DWORD size = sizeof(buffer);
		DWORD type = 0;
		if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size) ==
				ERROR_SUCCESS &&
			(type == REG_SZ || type == REG_EXPAND_SZ) && size >= sizeof(wchar_t)) {
			auto const chars = size / sizeof(wchar_t);
			if (chars > 0 && buffer[chars - 1] == L'\0')
				hash = fnv1a64(
					std::string_view(
						reinterpret_cast<char const*>(buffer),
						(chars - 1) * sizeof(wchar_t)),
					hash);
			else
				hash = fnv1a64(
					std::string_view(reinterpret_cast<char const*>(buffer), size), hash);
		}
	};
	auto read_dword = [&](wchar_t const* name) {
		DWORD value = 0;
		DWORD size = sizeof(value);
		DWORD type = 0;
		if (RegQueryValueExW(
				key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) ==
				ERROR_SUCCESS &&
			type == REG_DWORD)
			hash = fnv1a64_u32(value, hash);
	};
	read_string(L"CurrentBuildNumber");
	read_string(L"DisplayVersion");
	read_string(L"CompositionEditionID");
	read_dword(L"UBR");
	read_dword(L"CurrentMajorVersionNumber");
	read_dword(L"CurrentMinorVersionNumber");
	RegCloseKey(key);
	return hash;
}

std::uint64_t substitute_registry_fingerprint() {
	// RegEnumValueW order is not stable across boots. Collect then sort so M0
	// does not thrash on pure enumeration order changes.
	HKEY key = nullptr;
	if (RegOpenKeyExW(
			HKEY_LOCAL_MACHINE,
			L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes", 0,
			KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
		return 0;

	struct Entry {
		std::wstring name;
		std::wstring value;
	};
	std::vector<Entry> entries;
	DWORD index = 0;
	wchar_t name[256];
	wchar_t value[512];
	for (;;) {
		DWORD name_chars = static_cast<DWORD>(std::size(name));
		DWORD value_bytes = static_cast<DWORD>(sizeof(value));
		DWORD type = 0;
		auto const status = RegEnumValueW(
			key, index++, name, &name_chars, nullptr, &type,
			reinterpret_cast<LPBYTE>(value), &value_bytes);
		if (status == ERROR_NO_MORE_ITEMS)
			break;
		if (status != ERROR_SUCCESS)
			continue;
		if (type != REG_SZ && type != REG_EXPAND_SZ)
			continue;
		Entry entry;
		entry.name.assign(name, name_chars);
		auto value_chars = value_bytes / sizeof(wchar_t);
		if (value_chars > 0 && value[value_chars - 1] == L'\0')
			--value_chars;
		entry.value.assign(value, value_chars);
		entries.push_back(std::move(entry));
	}
	RegCloseKey(key);

	std::sort(entries.begin(), entries.end(), [](Entry const& left, Entry const& right) {
		if (left.name != right.name)
			return left.name < right.name;
		return left.value < right.value;
	});

	std::uint64_t hash = 14695981039346656037ull;
	for (auto const& entry : entries) {
		hash = fnv1a64(
			std::string_view(
				reinterpret_cast<char const*>(entry.name.data()),
				entry.name.size() * sizeof(wchar_t)),
			hash);
		hash = fnv1a64(
			std::string_view(
				reinterpret_cast<char const*>(entry.value.data()),
				entry.value.size() * sizeof(wchar_t)),
			hash);
	}
	return hash;
}

std::uint64_t provider_fingerprint(GdiFontResolver& resolver) {
	std::uint64_t hash = 14695981039346656037ull;
	hash = fnv1a64_u32(resolver.available() ? 1u : 0u, hash);
	hash = fnv1a64_u32(resolver.dwrite_available() ? 1u : 0u, hash);
	auto const description = resolver.dwrite_description();
	hash = fnv1a64(description, hash);
	return hash;
}

bool manifest_m0_equal(
	FontFamilyInputManifest const& left,
	FontFamilyInputManifest const& right) {
	if (left.manifest_contract_version != right.manifest_contract_version ||
	    left.provider_fingerprint != right.provider_fingerprint ||
	    left.os_build_fingerprint != right.os_build_fingerprint ||
	    left.substitute_registry_fingerprint != right.substitute_registry_fingerprint ||
	    left.gdi_family_names.size() != right.gdi_family_names.size())
		return false;
	for (std::size_t i = 0; i < left.gdi_family_names.size(); ++i)
		if (!ordinal_icase_equal(left.gdi_family_names[i], right.gdi_family_names[i]))
			return false;
	return true;
}

std::uint64_t unix_now() {
	using namespace std::chrono;
	return static_cast<std::uint64_t>(
		duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

void fill_non_name_fingerprints(
	FontFamilyInputManifest& manifest,
	GdiFontResolver& resolver) {
	manifest.manifest_contract_version = kFontFamilyObsManifestContractVersion;
	manifest.provider_fingerprint = provider_fingerprint(resolver);
	manifest.os_build_fingerprint = os_build_fingerprint();
	manifest.substitute_registry_fingerprint = substitute_registry_fingerprint();
}

void assess_probe_coverage(
	FontFamilyCatalogObservations const& observations,
	FontFamilyProbeObservation const& probe,
	ObservationCoverage& coverage) {
	if (!probe.success)
		return;
	++coverage.successful_probes;
	if (probe.face_ref == kFontFamilyInvalidFaceRef) {
		// No process entity and no face: device/bitmap selections that cannot
		// produce durable identity. Still counted as successful facts, but they
		// do not require a face_ref to admit a strong cache.
		if (probe.outcome.entity_token == 0) {
			++coverage.no_entity_probes;
			++coverage.covered_probes;
			return;
		}
		++coverage.missing_face_refs;
		if (coverage.reason.empty())
			coverage.reason = "successful probe with entity token missing face_ref";
		return;
	}
	if (probe.face_ref >= observations.faces.size()) {
		++coverage.invalid_face_refs;
		if (coverage.reason.empty())
			coverage.reason = "successful probe face_ref out of range";
		return;
	}
	if (!observations.faces[probe.face_ref].IsKnown()) {
		++coverage.unknown_faces;
		if (coverage.reason.empty())
			coverage.reason = "successful probe face identity is unknown";
		return;
	}
	++coverage.covered_probes;
}

} // namespace

char const* FontFamilyObsHitKindName(FontFamilyObsHitKind kind) noexcept {
	switch (kind) {
		case FontFamilyObsHitKind::MissNoPath: return "miss_no_path";
		case FontFamilyObsHitKind::MissLoad: return "miss_load";
		case FontFamilyObsHitKind::MissContract: return "miss_contract";
		case FontFamilyObsHitKind::MissM0: return "miss_m0";
		case FontFamilyObsHitKind::MissM1: return "miss_m1";
		case FontFamilyObsHitKind::MissIncomplete: return "miss_incomplete";
		case FontFamilyObsHitKind::MissCoverage: return "miss_coverage";
		case FontFamilyObsHitKind::MissLockTimeout: return "miss_lock_timeout";
		case FontFamilyObsHitKind::Hit: return "hit";
		case FontFamilyObsHitKind::Rebuilt: return "rebuilt";
		case FontFamilyObsHitKind::RebuildAborted: return "rebuild_aborted";
	}
	return "unknown";
}

ObservationCoverage AssessObservationCoverage(
	FontFamilyCatalogObservations const& observations) {
	ObservationCoverage coverage;
	for (auto const& seed : observations.seeds)
		for (auto const& probe : seed.rbiz)
			assess_probe_coverage(observations, probe, coverage);
	for (auto const& alias : observations.aliases)
		for (auto const& probe : alias.rbiz)
			assess_probe_coverage(observations, probe, coverage);
	coverage.strong = coverage.successful_probes == coverage.covered_probes &&
		coverage.missing_face_refs == 0 &&
		coverage.invalid_face_refs == 0 &&
		coverage.unknown_faces == 0;
	if (coverage.strong)
		coverage.reason.clear();
	else if (coverage.reason.empty())
		coverage.reason = "incomplete durable face coverage";
	return coverage;
}

void SanitizeLoadedTransientTokens(FontFamilyCatalogObservations& observations) {
	// After AssessObservationCoverage (entity_token classifies no_entity vs
	// missing_face_ref) and after M1. Durable probes keep face_ref; Derive uses
	// PhysicalVariantKey, not these cleared process tokens.
	for (auto& seed : observations.seeds) {
		for (auto& probe : seed.rbiz) {
			probe.outcome.entity_token = 0;
			for (auto& name : probe.informational_names)
				name.entity_token = 0;
		}
		for (auto& name : seed.win32_family_names)
			name.entity_token = 0;
	}
	for (auto& alias : observations.aliases) {
		for (auto& probe : alias.rbiz) {
			probe.outcome.entity_token = 0;
			for (auto& name : probe.informational_names)
				name.entity_token = 0;
		}
	}
}

FontFamilyInputManifest CollectWindowsFontFamilyManifest(
	GdiFontResolver& resolver,
	std::vector<std::string> const* gdi_family_names) {
	FontFamilyInputManifest manifest;
	fill_non_name_fingerprints(manifest, resolver);
	if (gdi_family_names) {
		manifest.gdi_family_names = sorted_unique_family_names(*gdi_family_names);
	} else {
		// EnumerateFamilies is one full EnumFontFamiliesExW pass (horizontal +
		// vertical faces in the same callback); only horizontal names are kept.
		manifest.gdi_family_names =
			sorted_unique_family_names(resolver.EnumerateFamilies());
	}
	return manifest;
}

agi::fs::path DefaultWindowsFontFamilyObsCachePath() {
	if (!config::path)
		return {};
	try {
		return config::path->Decode("?local/font_family_catalog/windows_gdi_obs.v1");
	}
	catch (...) {
		return {};
	}
}

namespace {

// Production default; tests may override via SetFontFamilyObsRebuildLockWaitMsForTest.
constexpr DWORD kDefaultRebuildLockWaitMs = 3000;
std::optional<std::uint32_t> g_rebuild_lock_wait_ms_for_test;

DWORD rebuild_lock_wait_ms() {
	if (g_rebuild_lock_wait_ms_for_test)
		return static_cast<DWORD>(*g_rebuild_lock_wait_ms_for_test);
	return kDefaultRebuildLockWaitMs;
}

/// Bounded cross-process rebuild lock. One mutex per cache path so multi-user
/// or multi-instance sessions do not serialize unrelated catalogs.
class ObservationCacheMutex {
	HANDLE mutex_ = nullptr;
	bool locked_ = false;

public:
	explicit ObservationCacheMutex(agi::fs::path const& cache_path) {
		auto const name = FontFamilyObsRebuildLockNameForPath(cache_path);
		mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
	}

	ObservationCacheMutex(ObservationCacheMutex const&) = delete;
	ObservationCacheMutex& operator=(ObservationCacheMutex const&) = delete;

	~ObservationCacheMutex() {
		if (locked_ && mutex_) {
			if (ReleaseMutex(mutex_) == 0) {
				LOG_W("font/family_catalog")
					<< "ReleaseMutex failed for observation rebuild lock (err="
					<< GetLastError() << ")";
			}
		}
		if (mutex_)
			CloseHandle(mutex_);
	}

	/// Wait up to the configured bound. WAIT_ABANDONED still grants ownership
	/// (peer crashed while holding the rebuild lock).
	bool Acquire() {
		if (!mutex_)
			return false;
		auto const status = WaitForSingleObject(mutex_, rebuild_lock_wait_ms());
		if (status == WAIT_OBJECT_0 || status == WAIT_ABANDONED) {
			locked_ = true;
			return true;
		}
		return false;
	}

	bool held() const noexcept { return locked_; }
};

} // namespace

std::wstring FontFamilyObsRebuildLockNameForPath(agi::fs::path const& cache_path) {
	// Stable, path-free mutex leaf: hash of the generic path string.
	std::string generic;
	try {
		generic = agi::fs::PathToGenericString(cache_path);
	}
	catch (...) {
		generic = agi::fs::PathToString(cache_path);
	}
	std::uint64_t hash = 14695981039346656037ull;
	for (unsigned char byte : generic) {
		hash ^= byte;
		hash *= 1099511628211ull;
	}
	wchar_t leaf[32] = {};
	_snwprintf_s(leaf, _TRUNCATE, L"%016llX", static_cast<unsigned long long>(hash));
	return std::wstring(L"Local\\Aegisub.FontFamilyObs.") + leaf;
}

void SetFontFamilyObsRebuildLockWaitMsForTest(std::optional<std::uint32_t> wait_ms) {
	g_rebuild_lock_wait_ms_for_test = wait_ms;
}

FontFamilyObsRepositoryResult LoadOrRebuildWindowsFontFamilyCatalog(
	GdiFontResolver& resolver,
	std::string_view locale,
	std::function<bool()> const& shutdown_requested,
	agi::fs::path const& cache_path) {
	FontFamilyObsRepositoryResult result;
	// M0 discovery snapshot before any rebuild probes. Family names may diverge
	// from the Observe seed set if fonts change mid-build; rebuild rewrites M0
	// from the observed seeds and retries once on mismatch.
	auto current_manifest = CollectWindowsFontFamilyManifest(resolver);

	// previous_for_incremental: complete disk body usable as Observe base when
	// non-name M0 fingerprints still match (family-name-only drift / M1 dirty).
	auto rebuild = [&](FontFamilyObsHitKind reason,
	                   std::vector<std::string> const* initial_seeds = nullptr,
	                   FontFamilyCatalogObservations const* previous_for_incremental =
	                       nullptr) {
		result.hit = reason;
		result.weak_m1 = false;
		result.wrote_cache = false;
		FontFamilyCatalogObservations observations;
		constexpr int kMaxObserveAttempts = 2;
		for (int attempt = 0; attempt < kMaxObserveAttempts; ++attempt) {
			// Prefer shared seeds on attempt 0; re-enumerate on retry so M0
			// drift mid-build is observed with a fresh full GDI pass.
			std::vector<std::string> owned_seeds;
			std::vector<std::string> const* seeds = nullptr;
			if (attempt == 0 && initial_seeds) {
				seeds = initial_seeds;
			} else {
				owned_seeds = resolver.EnumerateFamilies();
				seeds = &owned_seeds;
			}
			// Incremental only on first attempt with a trusted previous body.
			// Retry after mid-build M0 drift uses a full Observe.
			ObserveWindowsFontFamiliesOptions observe_options;
			observe_options.seeds = seeds;
			if (attempt == 0 && previous_for_incremental &&
			    previous_for_incremental->complete)
				observe_options.previous = previous_for_incremental;
			auto observe = ObserveWindowsFontFamilies(
				resolver, shutdown_requested, observe_options);
			observations = std::move(observe.observations);
			result.physical_probe_count = observe.physical_probe_count
				? observe.physical_probe_count
				: resolver.stats().physical_probe_count;
			if (observe.used_incremental) {
				LOG_I("font/family_catalog")
					<< "Incremental Observe: reused_seeds="
					<< observe.reused_seed_count
					<< " probed_seeds=" << observe.probed_seed_count
					<< " physical_probes=" << result.physical_probe_count;
			}
			if (!observations.complete) {
				result.hit = FontFamilyObsHitKind::RebuildAborted;
				result.catalog = {};
				result.face_count = 0;
				result.coverage = {};
				return result;
			}
			auto after_names = family_names_from_seeds(observations);
			if (after_names == current_manifest.gdi_family_names ||
				attempt + 1 == kMaxObserveAttempts) {
				// Publish M0 from the seed set that produced these observations so
				// disk facts stay self-consistent under the rebuild lock.
				current_manifest.gdi_family_names = std::move(after_names);
				fill_non_name_fingerprints(current_manifest, resolver);
				break;
			}
			LOG_I("font/family_catalog")
				<< "Observation M0 family set changed during rebuild; retrying once ("
				<< current_manifest.gdi_family_names.size() << " -> "
				<< after_names.size() << " names)";
			current_manifest.gdi_family_names = std::move(after_names);
			fill_non_name_fingerprints(current_manifest, resolver);
		}

		result.face_count = observations.faces.size();
		result.coverage = AssessObservationCoverage(observations);
		// complete && !strong: cold Derive still returns a catalog, but must not
		// overwrite a formal strong cache with weak coverage facts.
		if (!cache_path.empty() && result.coverage.strong) {
			FontFamilyObsStorePayload payload;
			payload.manifest = current_manifest;
			payload.observations = std::move(observations);
			payload.observation_contract_version =
				kFontFamilyObsObservationContractVersion;
			payload.derivation_contract_version =
				kFontFamilyObsDerivationContractVersion;
			payload.created_utc_unix = unix_now();
			auto const save = SaveFontFamilyObsStoreAtomic(cache_path, payload);
			result.wrote_cache = save == FontFamilyObsStoreStatus::Ok;
			if (!result.wrote_cache) {
				LOG_W("font/family_catalog")
					<< "Observation cache write refused: "
					<< FontFamilyObsStoreStatusName(save);
			}
			result.catalog = DeriveWindowsFontFamilyCatalog(payload.observations, locale);
			result.hit = FontFamilyObsHitKind::Rebuilt;
			return result;
		}
		if (!result.coverage.strong) {
			LOG_W("font/family_catalog")
				<< "weak_coverage: " << result.coverage.reason
				<< " (successful=" << result.coverage.successful_probes
				<< " covered=" << result.coverage.covered_probes
				<< " missing_face_refs=" << result.coverage.missing_face_refs
				<< " invalid_face_refs=" << result.coverage.invalid_face_refs
				<< " unknown_faces=" << result.coverage.unknown_faces
				<< "); not writing strong cache";
		}
		result.catalog = DeriveWindowsFontFamilyCatalog(observations, locale);
		result.hit = FontFamilyObsHitKind::Rebuilt;
		return result;
	};

	// Disk hit path only (no Observe). Fills `result` on success. On miss,
	// leaves a miss reason in result.hit for the caller to rebuild or return.
	auto try_admit_hit = [&]() -> bool {
		result.weak_m1 = false;
		result.wrote_cache = false;
		result.catalog = {};
		result.face_count = 0;
		result.coverage = {};
		result.physical_probe_count = resolver.stats().physical_probe_count;

		FontFamilyObsStorePayload loaded;
		result.store_status = TryLoadFontFamilyObsStore(cache_path, loaded);
		if (result.store_status != FontFamilyObsStoreStatus::Ok) {
			result.hit = FontFamilyObsHitKind::MissLoad;
			return false;
		}
		if (loaded.observation_contract_version !=
				kFontFamilyObsObservationContractVersion ||
			loaded.manifest.manifest_contract_version !=
				kFontFamilyObsManifestContractVersion) {
			result.hit = FontFamilyObsHitKind::MissContract;
			return false;
		}
		if (!loaded.observations.complete) {
			result.hit = FontFamilyObsHitKind::MissIncomplete;
			return false;
		}
		if (!manifest_m0_equal(loaded.manifest, current_manifest)) {
			result.hit = FontFamilyObsHitKind::MissM0;
			return false;
		}

		result.face_count = loaded.observations.faces.size();
		result.coverage = AssessObservationCoverage(loaded.observations);
		if (!result.coverage.strong) {
			result.hit = FontFamilyObsHitKind::MissCoverage;
			return false;
		}
		if (loaded.observations.faces.empty()) {
			result.weak_m1 = true;
			LOG_W("font/family_catalog")
				<< "Observation cache hit with weak M1 (faces=0); trust is M0-only";
		} else if (!ValidateWindowsFontFamilyFaceStamps(loaded.observations.faces)) {
			result.hit = FontFamilyObsHitKind::MissM1;
			return false;
		}

		SanitizeLoadedTransientTokens(loaded.observations);
		result.hit = FontFamilyObsHitKind::Hit;
		result.catalog = DeriveWindowsFontFamilyCatalog(loaded.observations, locale);
		if (loaded.derivation_contract_version !=
			kFontFamilyObsDerivationContractVersion) {
			loaded.derivation_contract_version = kFontFamilyObsDerivationContractVersion;
			loaded.created_utc_unix = unix_now();
			auto const save = SaveFontFamilyObsStoreAtomic(cache_path, loaded);
			result.wrote_cache = save == FontFamilyObsStoreStatus::Ok;
		}
		return true;
	};

	// Load a complete previous body for incremental Observe when only family
	// names / face stamps drifted. Provider/OS/substitutes mismatch forces full.
	auto load_incremental_base =
		[&](FontFamilyCatalogObservations& out) -> bool {
		if (cache_path.empty())
			return false;
		FontFamilyObsStorePayload loaded;
		if (TryLoadFontFamilyObsStore(cache_path, loaded) !=
		    FontFamilyObsStoreStatus::Ok)
			return false;
		if (loaded.observation_contract_version !=
		        kFontFamilyObsObservationContractVersion ||
		    loaded.manifest.manifest_contract_version !=
		        kFontFamilyObsManifestContractVersion)
			return false;
		if (!loaded.observations.complete)
			return false;
		// Non-name fingerprints must match; name-list drift is why we rebuild.
		if (loaded.manifest.provider_fingerprint !=
		        current_manifest.provider_fingerprint ||
		    loaded.manifest.os_build_fingerprint !=
		        current_manifest.os_build_fingerprint ||
		    loaded.manifest.substitute_registry_fingerprint !=
		        current_manifest.substitute_registry_fingerprint)
			return false;
		// Only strong snapshots are safe to reuse: weak bodies can look
		// "complete" after token sanitize and would skip probes incorrectly.
		if (!AssessObservationCoverage(loaded.observations).strong)
			return false;
		SanitizeLoadedTransientTokens(loaded.observations);
		out = std::move(loaded.observations);
		return out.complete && !out.seeds.empty();
	};

	if (cache_path.empty())
		return rebuild(FontFamilyObsHitKind::MissNoPath, nullptr, nullptr);

	// Common warm path: admit without taking the rebuild lock.
	if (try_admit_hit())
		return result;

	auto const first_miss = result.hit;
	LOG_I("font/family_catalog")
		<< "Observation cache miss (" << FontFamilyObsHitKindName(first_miss)
		<< (result.store_status != FontFamilyObsStoreStatus::Ok
			    ? std::string(", store=") +
				      FontFamilyObsStoreStatusName(result.store_status)
			    : std::string())
		<< "); acquiring rebuild lock";

	// Serialize full rebuilds across processes. Hold only for re-check + rebuild.
	ObservationCacheMutex rebuild_lock(cache_path);
	if (!rebuild_lock.Acquire()) {
		LOG_W("font/family_catalog")
			<< "Observation rebuild lock timeout ("
			<< rebuild_lock_wait_ms()
			<< " ms); re-checking disk hit without Observe";
		// Peer may have finished writing while we waited.
		current_manifest = CollectWindowsFontFamilyManifest(resolver);
		if (try_admit_hit())
			return result;
		result.hit = FontFamilyObsHitKind::MissLockTimeout;
		result.catalog = {};
		result.face_count = 0;
		result.coverage = {};
		result.physical_probe_count = resolver.stats().physical_probe_count;
		return result;
	}

	// Under lock: one full GDI enum feeds both M0 refresh and rebuild seeds so
	// the miss path does not pay EnumerateFamilies twice before Observe.
	auto locked_seeds = resolver.EnumerateFamilies();
	current_manifest = CollectWindowsFontFamilyManifest(resolver, &locked_seeds);
	if (try_admit_hit()) {
		LOG_I("font/family_catalog")
			<< "Observation cache hit after rebuild lock (peer finished first)";
		return result;
	}

	FontFamilyCatalogObservations incremental_base;
	FontFamilyCatalogObservations const* previous_ptr = nullptr;
	if (load_incremental_base(incremental_base))
		previous_ptr = &incremental_base;

	return rebuild(result.hit, &locked_seeds, previous_ptr);
}
