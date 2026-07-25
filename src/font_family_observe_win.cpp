#include "font_family_observe_win.h"

#include "font_family_obs_face_stamp_win.h"
#include "font_family_obs_internal_win.h"
#include "gdi_font_resolver.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using font_family_obs_internal::ascii_lower;
using font_family_obs_internal::ordinal_icase_equal;
using font_family_obs_internal::to_utf16;

struct PathFaceKey {
	std::string path_lower;
	int face_index = -1;

	bool operator==(PathFaceKey const&) const = default;
};

struct PathFaceKeyHash {
	std::size_t operator()(PathFaceKey const& key) const noexcept {
		std::size_t h = std::hash<std::string>{}(key.path_lower);
		h ^= static_cast<std::size_t>(key.face_index) + 0x9E3779B9u + (h << 6) + (h >> 2);
		return h;
	}
};

std::optional<FontFamilyFaceIdentity> read_face_identity_uncached(
	std::string_view local_file_path,
	int face_index) {
	if (local_file_path.empty() || face_index < 0)
		return std::nullopt;
	auto const wide_path = to_utf16(local_file_path);
	if (!wide_path)
		return std::nullopt;
	HANDLE const file = CreateFileW(
		wide_path->c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return std::nullopt;
	BY_HANDLE_FILE_INFORMATION information{};
	LARGE_INTEGER size{};
	bool const valid = GetFileInformationByHandle(file, &information) != FALSE &&
		GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0;
	CloseHandle(file);
	if (!valid)
		return std::nullopt;
	FontFamilyFaceIdentity identity;
	identity.volume_serial = information.dwVolumeSerialNumber;
	identity.file_index =
		(static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) |
		information.nFileIndexLow;
	identity.face_index = face_index;
	identity.size = static_cast<std::uint64_t>(size.QuadPart);
	identity.mtime_utc_100ns =
		(static_cast<std::uint64_t>(information.ftLastWriteTime.dwHighDateTime) << 32) |
		information.ftLastWriteTime.dwLowDateTime;
	return identity.volume_serial && identity.file_index ? std::optional(identity) : std::nullopt;
}

/// Per-Observe identity cache: avoids repeated CreateFileW for TTC faces that
/// share a path within one ObserveWindowsFontFamilies call. Stack-scoped so a
/// later Observe (invalidate/rebuild/comparator) re-opens files and never
/// reuses a stale process-wide identity. Memory only; never persisted (B4).
class FaceIdentityCache {
	std::unordered_map<PathFaceKey, FontFamilyFaceIdentity, PathFaceKeyHash> map_;

public:
	std::optional<FontFamilyFaceIdentity> read(
		std::string_view local_file_path,
		int face_index) {
		if (local_file_path.empty() || face_index < 0)
			return std::nullopt;
		PathFaceKey key{ascii_lower(local_file_path), face_index};
		auto const it = map_.find(key);
		if (it != map_.end())
			return it->second;
		auto identity = read_face_identity_uncached(local_file_path, face_index);
		if (!identity)
			return std::nullopt;
		auto const [inserted_it, inserted] = map_.emplace(std::move(key), *identity);
		(void)inserted;
		return inserted_it->second;
	}
};

std::uint32_t add_face_identity(
	std::vector<FontFamilyFaceIdentity>& faces,
	FontFamilyFaceIdentity const& identity) {
	auto const it = std::find(faces.begin(), faces.end(), identity);
	if (it != faces.end())
		return static_cast<std::uint32_t>(it - faces.begin());
	faces.push_back(identity);
	return static_cast<std::uint32_t>(faces.size() - 1);
}

void add_win32_name(
	std::vector<FontFamilyName>& names,
	DWriteLocalizedName const& name) {
	if (name.value.empty())
		return;
	auto const duplicate = std::find_if(names.begin(), names.end(),
		[&](FontFamilyName const& current) {
			return current.kind == FontFamilyNameKind::Win32Family &&
			       ordinal_icase_equal(current.value, name.value) &&
			       current.locale == name.locale;
		});
	if (duplicate == names.end())
		names.push_back({name.value, name.locale, FontFamilyNameKind::Win32Family});
}

void add_informational_name(
	std::vector<FontFamilyName>& names,
	DWriteLocalizedName const& name,
	FontFamilyNameKind kind,
	FontVariantOutcome const& outcome) {
	if (name.value.empty() || outcome.status != FontVariantStatus::Canonical ||
	    outcome.entity_token == 0 || outcome.role == FontVariantRole::Unknown)
		return;
	auto const duplicate = std::find_if(names.begin(), names.end(),
		[&](FontFamilyName const& current) {
			return current.kind == kind && current.value == name.value &&
			       current.locale == name.locale &&
			       current.entity_token == outcome.entity_token;
		});
	if (duplicate == names.end())
		names.push_back({
			name.value, name.locale, kind, outcome.entity_token, outcome.role});
}

FontFamilyProbeObservation collect_probe(
	GdiFontResolver& resolver,
	FaceIdentityCache& identity_cache,
	std::string_view family,
	int weight,
	bool italic,
	std::vector<FontFamilyName>* win32_family_names = nullptr,
	std::vector<FontFamilyFaceIdentity>* faces = nullptr) {
	FontFamilyProbeObservation observation;
	auto probe = resolver.ProbeWithSelection(
		family, weight, italic, DEFAULT_CHARSET, 0,
		[&](GdiFontSelectionView const& selected) {
			if (!selected.probe)
				return;
			if (faces) {
				if (auto identity = identity_cache.read(
						selected.local_file_path, selected.face_index))
					observation.face_ref = add_face_identity(*faces, *identity);
			}
			// Preserve the old builder's admission gate exactly: names from a
			// Regular selection without a successful, identifiable physical face
			// are metadata only and must not become catalog aliases.
			if (win32_family_names && selected.probe->success &&
			    selected.probe->outcome.entity_token != 0)
				for (auto const& name : selected.probe->win32_family_names)
					add_win32_name(*win32_family_names, name);
			if (!selected.dwrite_face || !selected.dwrite_bridge)
				return;
			for (auto const& name : selected.dwrite_bridge->GetFullNamesFromFace(
				     selected.dwrite_face))
				add_informational_name(
					observation.informational_names, name, FontFamilyNameKind::FullName,
					selected.probe->outcome);
			for (auto const& name : selected.dwrite_bridge->GetPostScriptNamesFromFace(
				     selected.dwrite_face))
				add_informational_name(
					observation.informational_names, name, FontFamilyNameKind::PostScript,
					selected.probe->outcome);
		});
	observation.outcome = std::move(probe.outcome);
	observation.matches_requested_family = probe.matches_requested_family;
	observation.success = probe.success;
	return observation;
}

/// Alias probe for non-seed candidates: physical selection + FaceIdentity.
/// Informational names are intentionally not collected for aliases (accepted
/// empty serialization; #8).
FontFamilyProbeObservation collect_alias_probe_with_identity(
	GdiFontResolver& resolver,
	FaceIdentityCache& identity_cache,
	std::string_view family,
	int weight,
	bool italic,
	std::vector<FontFamilyFaceIdentity>& faces) {
	FontFamilyProbeObservation observation;
	auto probe = resolver.ProbeWithSelection(
		family, weight, italic, DEFAULT_CHARSET, 0,
		[&](GdiFontSelectionView const& selected) {
			if (!selected.probe)
				return;
			if (auto identity = identity_cache.read(
					selected.local_file_path, selected.face_index))
				observation.face_ref = add_face_identity(faces, *identity);
		});
	observation.outcome = std::move(probe.outcome);
	observation.matches_requested_family = probe.matches_requested_family;
	observation.success = probe.success;
	return observation;
}

/// Copy durable selection facts from a seed probe. Do not copy
/// informational_names (alias rows do not carry Full/PostScript metadata).
FontFamilyProbeObservation copy_seed_probe_for_alias(
	FontFamilyProbeObservation const& seed_probe) {
	FontFamilyProbeObservation observation;
	observation.outcome = seed_probe.outcome;
	observation.matches_requested_family = seed_probe.matches_requested_family;
	observation.success = seed_probe.success;
	observation.face_ref = seed_probe.face_ref;
	return observation;
}

bool contains_alias(
	std::vector<FontFamilyAliasObservation> const& aliases,
	std::string_view candidate) {
	return std::any_of(aliases.begin(), aliases.end(),
		[&](FontFamilyAliasObservation const& alias) {
			return ordinal_icase_equal(alias.candidate_name, candidate);
		});
}

/// Alias candidates that are a seed's enumerated name or a Win32 family spelling
/// collected from that seed's Regular face reuse the seed's RBIZ probes. Those
/// names already selected the same GDI family topology; re-probing them would
/// add 4 physical probes per spelling without new facts.
FontFamilySeedObservation const* find_seed_for_alias_candidate(
	std::vector<FontFamilySeedObservation> const& seeds,
	std::string_view candidate) {
	for (auto const& seed : seeds) {
		if (ordinal_icase_equal(seed.seed_family_name, candidate))
			return &seed;
		for (auto const& name : seed.win32_family_names) {
			if (name.kind == FontFamilyNameKind::Win32Family &&
			    ordinal_icase_equal(name.value, candidate))
				return &seed;
		}
	}
	return nullptr;
}

/// Initialize COM MTA on worker threads before DWrite factory construction.
/// S_OK/S_FALSE both require CoUninitialize. RPC_E_CHANGED_MODE means the
/// thread already has a different apartment (e.g. STA); COM is usable, do not
/// uninit. Other failures leave DWrite to degrade to GDI-only metadata.
struct ScopedComMta {
	HRESULT hr = E_FAIL;
	bool should_uninit = false;

	ScopedComMta() {
		hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		should_uninit = SUCCEEDED(hr);
	}

	~ScopedComMta() {
		if (should_uninit)
			CoUninitialize();
	}

	ScopedComMta(ScopedComMta const&) = delete;
	ScopedComMta& operator=(ScopedComMta const&) = delete;
};

/// Hard cap on parallel GDI/DWrite workers (resource contention, not correctness).
constexpr unsigned kObserveMaxWorkers = 8;
/// If more than this many seeds need a fresh probe, fall back to full Observe.
/// Also falls back when dirty ratio exceeds kIncrementalDirtyRatio.
constexpr std::size_t kIncrementalMaxProbeSeeds = 64;
constexpr double kIncrementalDirtyRatio = 0.25;

unsigned ResolveWorkerCount(unsigned requested, std::size_t seed_count) {
	if (seed_count == 0)
		return 1;
	unsigned worker_count = requested;
	if (worker_count == 0) {
		worker_count = std::thread::hardware_concurrency();
		if (worker_count < 1)
			worker_count = 1;
	}
	if (worker_count > kObserveMaxWorkers)
		worker_count = kObserveMaxWorkers;
	if (worker_count > seed_count)
		worker_count = static_cast<unsigned>(seed_count);
	if (worker_count < 1)
		worker_count = 1;
	return worker_count;
}

FontFamilySeedObservation ObserveOneSeed(
	GdiFontResolver& local_resolver,
	FaceIdentityCache& local_cache,
	std::vector<FontFamilyFaceIdentity>& local_faces,
	std::string const& seed) {
	FontFamilySeedObservation observed_seed;
	observed_seed.seed_family_name = seed;
	observed_seed.profile_matches_requested_family = true;
	for (std::size_t index = 0; index < observed_seed.rbiz.size(); ++index) {
		bool const italic = index >= 2;
		bool const bold = (index & 1u) != 0;
		auto observation = collect_probe(
			local_resolver, local_cache, seed, bold ? FW_BOLD : FW_NORMAL, italic,
			index == 0 ? &observed_seed.win32_family_names : nullptr,
			&local_faces);
		observed_seed.profile_matches_requested_family =
			observed_seed.profile_matches_requested_family && observation.success &&
			observation.matches_requested_family &&
			observation.outcome.entity_token != 0;
		observed_seed.rbiz[index] = std::move(observation);
	}
	return observed_seed;
}

/// Cheap durable-shape check so a weak/crafted previous cannot enter reuse
/// (mirrors AssessObservationCoverage success rules without the full score).
bool PreviousObservationsLookDurable(
	FontFamilyCatalogObservations const& previous) {
	auto check_probe = [&](FontFamilyProbeObservation const& probe) {
		if (!probe.success)
			return true;
		if (probe.face_ref == kFontFamilyInvalidFaceRef) {
			// success + token without face is not durable (missing_face_ref).
			return probe.outcome.entity_token == 0;
		}
		if (probe.face_ref >= previous.faces.size())
			return false;
		return previous.faces[probe.face_ref].IsKnown();
	};
	for (auto const& seed : previous.seeds)
		for (auto const& probe : seed.rbiz)
			if (!check_probe(probe))
				return false;
	for (auto const& alias : previous.aliases)
		for (auto const& probe : alias.rbiz)
			if (!check_probe(probe))
				return false;
	return true;
}

/// Reuse only when every referenced durable face still matches live size/mtime.
/// Probes without face_ref are allowed (bitmap / no-entity); strong previous
/// coverage guarantees those were not "token without face" holes.
/// Stamp checks are on-demand via validator cache (not a full faces[] scan).
bool SeedFacesStillLive(
	FontFamilySeedObservation const& seed,
	std::vector<FontFamilyFaceIdentity> const& previous_faces,
	FontFamilyFaceStampValidator& stamps) {
	for (auto const& probe : seed.rbiz) {
		if (probe.face_ref == kFontFamilyInvalidFaceRef)
			continue;
		if (probe.face_ref >= previous_faces.size())
			return false;
		if (!stamps.matches(previous_faces[probe.face_ref]))
			return false;
	}
	return true;
}

/// Ordinal case-insensitive wide key map for previous seed lookup (O(log n)).
struct OrdinalIcaseWideLess {
	bool operator()(std::wstring const& left, std::wstring const& right) const {
		return CompareStringOrdinal(
			left.data(), static_cast<int>(left.size()),
			right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
	}
};

std::map<std::wstring, std::size_t, OrdinalIcaseWideLess> BuildPreviousSeedIndex(
	std::vector<FontFamilySeedObservation> const& previous_seeds) {
	std::map<std::wstring, std::size_t, OrdinalIcaseWideLess> index;
	for (std::size_t i = 0; i < previous_seeds.size(); ++i) {
		auto wide = to_utf16(previous_seeds[i].seed_family_name);
		if (!wide || wide->empty())
			continue;
		// First spelling wins if duplicates (should not happen in a clean store).
		index.emplace(std::move(*wide), i);
	}
	return index;
}

FontFamilySeedObservation const* FindPreviousSeed(
	std::map<std::wstring, std::size_t, OrdinalIcaseWideLess> const& index,
	std::vector<FontFamilySeedObservation> const& previous_seeds,
	std::string_view name) {
	auto wide = to_utf16(name);
	if (!wide)
		return nullptr;
	auto const it = index.find(*wide);
	if (it == index.end() || it->second >= previous_seeds.size())
		return nullptr;
	return &previous_seeds[it->second];
}

FontFamilySeedObservation RemapSeedFaces(
	FontFamilySeedObservation seed,
	std::vector<FontFamilyFaceIdentity> const& previous_faces,
	std::vector<FontFamilyFaceIdentity>& out_faces) {
	for (auto& probe : seed.rbiz) {
		if (probe.face_ref == kFontFamilyInvalidFaceRef)
			continue;
		if (probe.face_ref >= previous_faces.size()) {
			probe.face_ref = kFontFamilyInvalidFaceRef;
			continue;
		}
		probe.face_ref = add_face_identity(out_faces, previous_faces[probe.face_ref]);
	}
	return seed;
}

void AppendAliasPass(
	GdiFontResolver& resolver,
	std::function<bool()> const& shutdown_requested,
	FontFamilyCatalogObservations& observations,
	std::uint64_t& alias_probes_out) {
	FaceIdentityCache identity_cache;
	auto const alias_probes_before = resolver.stats().physical_probe_count;
	for (auto const& seed : observations.seeds) {
		std::vector<std::string> candidates{seed.seed_family_name};
		for (auto const& name : seed.win32_family_names)
			if (std::none_of(candidates.begin(), candidates.end(),
				    [&](std::string const& current) {
					    return ordinal_icase_equal(current, name.value);
				    }))
				candidates.push_back(name.value);
		for (auto const& candidate : candidates) {
			if (candidate.empty() || contains_alias(observations.aliases, candidate))
				continue;
			if (shutdown_requested && shutdown_requested()) {
				observations.complete = false;
				alias_probes_out =
					resolver.stats().physical_probe_count - alias_probes_before;
				return;
			}
			FontFamilyAliasObservation alias;
			alias.candidate_name = candidate;
			if (auto const* matching_seed =
					find_seed_for_alias_candidate(observations.seeds, candidate)) {
				for (std::size_t index = 0; index < alias.rbiz.size(); ++index)
					alias.rbiz[index] = copy_seed_probe_for_alias(
						matching_seed->rbiz[index]);
			} else {
				for (std::size_t index = 0; index < alias.rbiz.size(); ++index) {
					bool const italic = index >= 2;
					bool const bold = (index & 1u) != 0;
					alias.rbiz[index] = collect_alias_probe_with_identity(
						resolver, identity_cache, candidate, bold ? FW_BOLD : FW_NORMAL,
						italic, observations.faces);
				}
			}
			observations.aliases.push_back(std::move(alias));
		}
	}
	alias_probes_out = resolver.stats().physical_probe_count - alias_probes_before;
	observations.complete = true;
}

/// Probe only the listed seed names (parallel), return ordered seeds + faces.
/// On abort, complete remains false and seeds may be partial.
struct ProbeSeedsResult {
	std::vector<FontFamilySeedObservation> seeds;
	std::vector<FontFamilyFaceIdentity> faces;
	std::uint64_t physical_probe_count = 0;
	bool aborted = false;
};

ProbeSeedsResult ProbeSeedList(
	GdiFontResolver& resolver,
	std::function<bool()> const& shutdown_requested,
	std::vector<std::string> const& seeds,
	unsigned worker_count_request) {
	ProbeSeedsResult out;
	if (seeds.empty())
		return out;

	unsigned const worker_count = ResolveWorkerCount(worker_count_request, seeds.size());
	struct WorkerOut {
		std::vector<FontFamilySeedObservation> seeds;
		std::vector<FontFamilyFaceIdentity> faces;
	};
	std::vector<WorkerOut> worker_outs(worker_count);
	std::atomic<bool> abort{false};
	std::atomic<std::uint64_t> seed_physical_probes{0};

	if (worker_count == 1) {
		FaceIdentityCache identity_cache;
		auto const probes_before = resolver.stats().physical_probe_count;
		worker_outs[0].seeds.reserve(seeds.size());
		for (auto const& seed : seeds) {
			if (shutdown_requested && shutdown_requested()) {
				abort.store(true);
				break;
			}
			worker_outs[0].seeds.push_back(
				ObserveOneSeed(resolver, identity_cache, worker_outs[0].faces, seed));
		}
		seed_physical_probes.store(
			resolver.stats().physical_probe_count - probes_before);
	} else {
		std::vector<std::thread> workers;
		workers.reserve(worker_count);
		for (unsigned w = 0; w < worker_count; ++w) {
			workers.emplace_back([&, w] {
				ScopedComMta com;
				GdiFontResolver local_resolver;
				if (!local_resolver.available()) {
					abort.store(true);
					return;
				}
				FaceIdentityCache local_cache;
				auto& wout = worker_outs[w];
				wout.seeds.reserve((seeds.size() / worker_count) + 1);
				for (std::size_t i = w; i < seeds.size(); i += worker_count) {
					if (abort.load())
						return;
					if (shutdown_requested && shutdown_requested()) {
						abort.store(true);
						return;
					}
					wout.seeds.push_back(ObserveOneSeed(
						local_resolver, local_cache, wout.faces, seeds[i]));
				}
				seed_physical_probes.fetch_add(
					local_resolver.stats().physical_probe_count,
					std::memory_order_relaxed);
			});
		}
		for (auto& worker : workers)
			worker.join();
	}

	if (abort.load()) {
		out.aborted = true;
		return out;
	}

	out.seeds.resize(seeds.size());
	for (unsigned w = 0; w < worker_count; ++w) {
		auto& wout = worker_outs[w];
		std::vector<std::uint32_t> remap(wout.faces.size(), kFontFamilyInvalidFaceRef);
		for (std::size_t fi = 0; fi < wout.faces.size(); ++fi)
			remap[fi] = add_face_identity(out.faces, wout.faces[fi]);

		std::size_t local_index = 0;
		for (std::size_t i = w; i < seeds.size(); i += worker_count) {
			auto seed = std::move(wout.seeds[local_index++]);
			for (auto& probe : seed.rbiz) {
				if (probe.face_ref != kFontFamilyInvalidFaceRef &&
				    probe.face_ref < remap.size())
					probe.face_ref = remap[probe.face_ref];
			}
			out.seeds[i] = std::move(seed);
		}
	}
	out.physical_probe_count = seed_physical_probes.load(std::memory_order_relaxed);
	return out;
}

} // namespace

ObserveWindowsFontFamiliesResult ObserveWindowsFontFamilies(
	GdiFontResolver& resolver,
	std::function<bool()> const& shutdown_requested,
	ObserveWindowsFontFamiliesOptions options) {
	ObserveWindowsFontFamiliesResult result;
	auto& observations = result.observations;

	// Calling thread: serial seed probes + alias pass reuse `resolver` (and its
	// DWrite factory). Prefer an explicit MTA here so we do not rely only on
	// GUI STA / gtest implicit COM. RPC_E_CHANGED_MODE is fine (already inited).
	// Note: this cannot repair a factory that was already created on a thread
	// without COM; workers construct resolvers after their own ScopedComMta.
	ScopedComMta caller_com;

	std::vector<std::string> owned_seeds;
	std::vector<std::string> const* seeds_ptr = options.seeds;
	if (!seeds_ptr) {
		owned_seeds = resolver.EnumerateFamilies();
		seeds_ptr = &owned_seeds;
	}
	auto const& seeds = *seeds_ptr;
	if (seeds.empty()) {
		observations.complete = true;
		return result;
	}

	// --- Optional incremental: reuse stamp-live previous seeds; probe the rest ---
	// Assumptions (enforced by repository before passing previous):
	// - previous is complete and had strong coverage when loaded
	// - process tokens may already be sanitized (Derive uses face_ref)
	// - file identity is size+mtime only (same as Hit M1); content-only edits
	//   with unchanged stamps are out of scope.
	std::vector<FontFamilySeedObservation const*> reuse_from_previous(seeds.size(), nullptr);
	std::vector<std::string> probe_names;
	probe_names.reserve(seeds.size());
	std::vector<std::size_t> probe_slots;
	probe_slots.reserve(seeds.size());
	bool try_incremental = options.previous && options.previous->complete &&
		!options.previous->seeds.empty() &&
		PreviousObservationsLookDurable(*options.previous);

	if (try_incremental) {
		auto const prev_index = BuildPreviousSeedIndex(options.previous->seeds);
		FontFamilyFaceStampValidator stamps;
		for (std::size_t i = 0; i < seeds.size(); ++i) {
			auto const* prev_seed = FindPreviousSeed(
				prev_index, options.previous->seeds, seeds[i]);
			if (prev_seed &&
			    SeedFacesStillLive(*prev_seed, options.previous->faces, stamps)) {
				reuse_from_previous[i] = prev_seed;
				++result.reused_seed_count;
			} else {
				probe_names.push_back(seeds[i]);
				probe_slots.push_back(i);
			}
		}

		auto const dirty = probe_names.size();
		auto const dirty_limit = std::max(
			kIncrementalMaxProbeSeeds,
			static_cast<std::size_t>(seeds.size() * kIncrementalDirtyRatio));
		if (dirty > dirty_limit || result.reused_seed_count == 0) {
			// Too much churn (or nothing reusable): full Observe.
			try_incremental = false;
			result.reused_seed_count = 0;
			std::fill(reuse_from_previous.begin(), reuse_from_previous.end(), nullptr);
			probe_names.clear();
			probe_slots.clear();
		}
	}

	if (!try_incremental) {
		probe_names.assign(seeds.begin(), seeds.end());
		probe_slots.resize(seeds.size());
		for (std::size_t i = 0; i < seeds.size(); ++i)
			probe_slots[i] = i;
	}

	result.probed_seed_count = static_cast<std::uint32_t>(probe_names.size());
	result.used_incremental = try_incremental && result.reused_seed_count > 0;

	// Place reused seeds first so face table prefers stable previous identities.
	observations.seeds.resize(seeds.size());
	if (result.used_incremental) {
		for (std::size_t i = 0; i < seeds.size(); ++i) {
			if (!reuse_from_previous[i])
				continue;
			observations.seeds[i] = RemapSeedFaces(
				*reuse_from_previous[i], options.previous->faces, observations.faces);
		}
	}

	auto probed = ProbeSeedList(
		resolver, shutdown_requested, probe_names, options.worker_count);
	if (probed.aborted)
		return result;

	// Merge freshly probed faces into the unified table and fill seed slots.
	std::vector<std::uint32_t> probe_face_remap(
		probed.faces.size(), kFontFamilyInvalidFaceRef);
	for (std::size_t fi = 0; fi < probed.faces.size(); ++fi)
		probe_face_remap[fi] = add_face_identity(observations.faces, probed.faces[fi]);

	for (std::size_t pi = 0; pi < probe_slots.size(); ++pi) {
		auto seed = std::move(probed.seeds[pi]);
		for (auto& probe : seed.rbiz) {
			if (probe.face_ref != kFontFamilyInvalidFaceRef &&
			    probe.face_ref < probe_face_remap.size())
				probe.face_ref = probe_face_remap[probe.face_ref];
		}
		observations.seeds[probe_slots[pi]] = std::move(seed);
	}

	// Alias pass always rebuilds from the final seed set (owned spellings copy).
	std::uint64_t alias_probes = 0;
	AppendAliasPass(resolver, shutdown_requested, observations, alias_probes);
	if (!observations.complete)
		return result;

	result.physical_probe_count = probed.physical_probe_count + alias_probes;
	return result;
}
