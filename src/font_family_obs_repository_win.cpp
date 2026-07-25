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

struct RegistryValueSnapshot {
	std::wstring name;
	DWORD type = REG_NONE;
	std::vector<unsigned char> data;
};

std::uint64_t hash_registry_key(
	HKEY root,
	wchar_t const* path,
	REGSAM access,
	std::uint32_t key_tag,
	std::uint64_t hash) {
	hash = fnv1a64_u32(key_tag, hash);
	HKEY key = nullptr;
	auto const open_status = RegOpenKeyExW(root, path, 0, access, &key);
	hash = fnv1a64_u32(static_cast<std::uint32_t>(open_status), hash);
	if (open_status != ERROR_SUCCESS)
		return hash;

	DWORD value_count = 0;
	DWORD max_name_chars = 0;
	DWORD max_data_bytes = 0;
	FILETIME initial_last_write{};
	auto const info_status = RegQueryInfoKeyW(
		key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
		&value_count, &max_name_chars, &max_data_bytes, nullptr,
		&initial_last_write);
	hash = fnv1a64_u32(static_cast<std::uint32_t>(info_status), hash);
	if (info_status != ERROR_SUCCESS) {
		RegCloseKey(key);
		return hash;
	}
	constexpr DWORD kMaxRegistryValues = 100000;
	constexpr DWORD kMaxRegistryNameChars = 32767;
	constexpr DWORD kMaxRegistryDataBytes = 4u * 1024u * 1024u;
	hash = fnv1a64_u32(value_count, hash);
	hash = fnv1a64_u32(max_name_chars, hash);
	hash = fnv1a64_u32(max_data_bytes, hash);
	hash = fnv1a64_u32(initial_last_write.dwLowDateTime, hash);
	hash = fnv1a64_u32(initial_last_write.dwHighDateTime, hash);
	if (value_count > kMaxRegistryValues ||
	    max_name_chars > kMaxRegistryNameChars ||
	    max_data_bytes > kMaxRegistryDataBytes) {
		RegCloseKey(key);
		return hash;
	}

	std::vector<RegistryValueSnapshot> entries;
	entries.reserve(value_count);
	std::vector<wchar_t> name(static_cast<std::size_t>(max_name_chars) + 1);
	std::vector<unsigned char> data(
		std::max<std::size_t>(max_data_bytes, 1));
	for (DWORD index = 0;; ++index) {
		LONG status = ERROR_MORE_DATA;
		DWORD name_chars = 0;
		DWORD data_bytes = 0;
		DWORD type = REG_NONE;
		for (int retry = 0; retry < 8 && status == ERROR_MORE_DATA; ++retry) {
			name_chars = static_cast<DWORD>(name.size());
			data_bytes = static_cast<DWORD>(data.size());
			status = RegEnumValueW(
				key, index, name.data(), &name_chars, nullptr, &type,
				data.data(), &data_bytes);
			if (status == ERROR_MORE_DATA) {
				auto const next_name_size = std::min<std::size_t>(
					std::max(name.size() * 2,
						static_cast<std::size_t>(name_chars) + 1),
					static_cast<std::size_t>(kMaxRegistryNameChars) + 1);
				auto const next_data_size = std::min<std::size_t>(
					std::max(data.size() * 2,
						static_cast<std::size_t>(data_bytes)),
					kMaxRegistryDataBytes);
				if (next_name_size == name.size() &&
				    next_data_size == data.size())
					break;
				name.resize(next_name_size);
				data.resize(next_data_size);
			}
		}
		if (status == ERROR_NO_MORE_ITEMS)
			break;
		if (status != ERROR_SUCCESS) {
			hash = fnv1a64_u32(static_cast<std::uint32_t>(status), hash);
			continue;
		}
		RegistryValueSnapshot entry;
		entry.name.assign(name.data(), name_chars);
		entry.type = type;
		entry.data.assign(data.begin(), data.begin() + data_bytes);
		entries.push_back(std::move(entry));
	}
	FILETIME final_last_write{};
	auto const final_info_status = RegQueryInfoKeyW(
		key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
		nullptr, nullptr, nullptr, nullptr, &final_last_write);
	hash = fnv1a64_u32(static_cast<std::uint32_t>(final_info_status), hash);
	if (final_info_status == ERROR_SUCCESS) {
		hash = fnv1a64_u32(final_last_write.dwLowDateTime, hash);
		hash = fnv1a64_u32(final_last_write.dwHighDateTime, hash);
	}
	RegCloseKey(key);

	// Registry enumeration order is not stable. Sort names case-insensitively,
	// matching registry value-name semantics, then hash length-delimited bytes.
	std::sort(entries.begin(), entries.end(), [](auto const& left, auto const& right) {
		auto const order = CompareStringOrdinal(
			left.name.data(), static_cast<int>(left.name.size()),
			right.name.data(), static_cast<int>(right.name.size()), TRUE);
		if (order != CSTR_EQUAL)
			return order == CSTR_LESS_THAN;
		if (left.type != right.type)
			return left.type < right.type;
		return left.data < right.data;
	});
	for (auto const& entry : entries) {
		hash = fnv1a64_u32(static_cast<std::uint32_t>(entry.name.size()), hash);
		hash = fnv1a64(std::string_view(
			reinterpret_cast<char const*>(entry.name.data()),
			entry.name.size() * sizeof(wchar_t)), hash);
		hash = fnv1a64_u32(entry.type, hash);
		hash = fnv1a64_u32(static_cast<std::uint32_t>(entry.data.size()), hash);
		if (!entry.data.empty())
			hash = fnv1a64(std::string_view(
				reinterpret_cast<char const*>(entry.data.data()), entry.data.size()), hash);
	}
	return hash;
}

std::uint64_t font_registry_fingerprint() {
	// Hash selection inputs from both persistent installation scopes. Only the
	// digest is persisted; registry strings (including any file paths) are not.
	constexpr auto machine_access = KEY_READ | KEY_WOW64_64KEY;
	constexpr auto user_access = KEY_READ;
	std::uint64_t hash = 14695981039346656037ull;
	auto add_scope = [&](HKEY root, REGSAM access, std::uint32_t tag_base) {
		hash = hash_registry_key(
			root, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
			access, tag_base + 0, hash);
		hash = hash_registry_key(
			root, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes",
			access, tag_base + 1, hash);
		hash = hash_registry_key(
			root, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink",
			access, tag_base + 2, hash);
	};
	add_scope(HKEY_LOCAL_MACHINE, machine_access, 0x1000u);
	add_scope(HKEY_CURRENT_USER, user_access, 0x2000u);
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

bool manifest_m0_equal_impl(
	FontFamilyInputManifest const& left,
	FontFamilyInputManifest const& right) {
	if (left.manifest_contract_version != right.manifest_contract_version ||
	    left.provider_fingerprint != right.provider_fingerprint ||
	    left.os_build_fingerprint != right.os_build_fingerprint ||
	    left.font_registry_fingerprint != right.font_registry_fingerprint ||
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
	manifest.font_registry_fingerprint = font_registry_fingerprint();
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

bool WindowsFontFamilyManifestsMatchM0(
	FontFamilyInputManifest const& left,
	FontFamilyInputManifest const& right) {
	return manifest_m0_equal_impl(left, right);
}

bool WindowsFontFamilyObservationM0Stable(
	FontFamilyInputManifest const& before,
	FontFamilyInputManifest const& after,
	std::vector<std::string> const& observed_family_names) {
	if (!manifest_m0_equal_impl(before, after))
		return false;
	auto observed = before;
	observed.gdi_family_names = observed_family_names;
	return manifest_m0_equal_impl(before, observed);
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
		return config::path->Decode(
			"?local/font_family_catalog/windows-font-observations.afco");
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
	// M0 discovery snapshot before any rebuild probes. The warm hit uses this
	// directly; rebuild takes a fresh before/after pair under the lock.
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
		bool stable_m0 = false;
		std::uint64_t total_physical_probe_count = 0;
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
			auto const before_manifest =
				CollectWindowsFontFamilyManifest(resolver, seeds);
			// Incremental only on first attempt with a trusted previous body.
			// Recheck the manifest used to admit that body: external font changes
			// do not take our rebuild lock. Retry or pre-Observe drift uses a full
			// Observe so old facts are never published under a new M0.
			ObserveWindowsFontFamiliesOptions observe_options;
			observe_options.seeds = seeds;
			if (attempt == 0 && previous_for_incremental &&
			    previous_for_incremental->complete &&
			    WindowsFontFamilyManifestsMatchM0(
				    current_manifest, before_manifest))
				observe_options.previous = previous_for_incremental;
			auto const resolver_probes_before =
				resolver.stats().physical_probe_count;
			auto observe = ObserveWindowsFontFamilies(
				resolver, shutdown_requested, observe_options);
			observations = std::move(observe.observations);
			auto const resolver_probe_delta =
				resolver.stats().physical_probe_count - resolver_probes_before;
			total_physical_probe_count += observe.physical_probe_count
				? observe.physical_probe_count
				: resolver_probe_delta;
			result.physical_probe_count = total_physical_probe_count;
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
			auto const observed_names = family_names_from_seeds(observations);
			auto after_manifest = CollectWindowsFontFamilyManifest(resolver);
			stable_m0 = WindowsFontFamilyObservationM0Stable(
				before_manifest, after_manifest, observed_names);
			current_manifest = std::move(after_manifest);
			if (stable_m0)
				break;
			if (attempt + 1 < kMaxObserveAttempts) {
				LOG_I("font/family_catalog")
					<< "Observation M0 changed during rebuild; retrying once";
			}
		}

		result.face_count = observations.faces.size();
		result.coverage = AssessObservationCoverage(observations);
		if (!stable_m0) {
			LOG_W("font/family_catalog")
				<< "Observation M0 remained unstable after retry; not writing cache";
			result.catalog = DeriveWindowsFontFamilyCatalog(observations, locale);
			result.hit = FontFamilyObsHitKind::Rebuilt;
			return result;
		}
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
		if (!WindowsFontFamilyManifestsMatchM0(loaded.manifest, current_manifest)) {
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
	// names / face stamps drifted. Provider/OS/registry mismatch forces full.
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
		    loaded.manifest.font_registry_fingerprint !=
		        current_manifest.font_registry_fingerprint)
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
