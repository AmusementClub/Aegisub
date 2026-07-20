#include "libass_runtime.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/native_library.h>

#ifdef _WIN32
#include <libaegisub/charset_conv_win.h>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h defines GetMessage as GetMessageW, which breaks err.GetMessage().
#ifdef GetMessage
#undef GetMessage
#endif
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace stdfs = std::filesystem;

namespace libass::runtime {
namespace {

constexpr char kLogTag[] = "subtitle/provider/libass/runtime";

struct RuntimeState {
	Api api;
	int loaded_version = -1;
	std::string loaded_path;
};

// All cross-TU mutable state lives on an intentionally leaked heap singleton so
// provider / cache-queue static destructors can still call GetApi() after other
// translation units have begun static teardown.
struct RuntimeGlobals {
	std::mutex mutex;
	RuntimeState state;
	bool load_attempted = false;
	bool load_complete = false;
	std::string load_error;
};

RuntimeGlobals& Globals() {
	static RuntimeGlobals *const g = new RuntimeGlobals;
	return *g;
}

// Marker used only for GetModuleHandleEx / dladdr module localization.
void ModuleAddressAnchor() {}

std::string GetCurrentModuleDirectory() {
#ifdef _WIN32
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&ModuleAddressAnchor),
		&module) || !module) {
		return {};
	}
	std::wstring path(32768, L'\0');
	auto len = GetModuleFileNameW(module, &path[0], static_cast<DWORD>(path.size()));
	if (!len)
		return {};
	path.resize(len);
	return agi::fs::PathToString(stdfs::path(path).parent_path());
#else
	Dl_info info{};
	if (!dladdr(reinterpret_cast<void *>(&ModuleAddressAnchor), &info) || !info.dli_fname)
		return {};
	return agi::fs::PathToString(stdfs::path{info.dli_fname}.parent_path());
#endif
}

// Logical library names tried in order. CachedLibrary only accepts one name, so
// the multi-candidate sequence lives here (Unix needs SONAME + unversioned).
std::vector<std::string> GetLibraryCandidates() {
#if defined(_WIN32)
	// Windows ABI filename is ass.dll (no SONAME variants).
	return {"ass"};
#elif defined(__APPLE__)
	// Prefer versioned SONAME, then unversioned for Homebrew/dev-package layouts.
	// BuildLibraryNameVariations expands each into lib-prefixed / .dylib forms.
	return {"ass.9", "ass"};
#else
	// Prefer ABI SONAME (runtime-only packages), then unversioned libass.so.
	return {"ass.so.9", "ass"};
#endif
}

std::vector<std::string> GetAppLocalSearchBases() {
	std::vector<std::string> bases;
	auto add_unique = [&](std::string base) {
		if (base.empty())
			return;
		for (auto const& existing : bases) {
			if (existing == base)
				return;
		}
		bases.push_back(std::move(base));
	};

	// Module directory first so an installed aegisub_core_c_api.dll finds
	// bin/runtimes/ass.dll even when the host EXE lives elsewhere.
	add_unique(GetCurrentModuleDirectory());

	// Also probe the host executable layout (portable Aegisub, smokes).
	// BuildLibraryLoadProbes without an explicit directory uses the exe dir;
	// we materialize that by reusing the no-directory overload when building
	// probes (see CollectAppLocalProbes).
	return bases;
}

std::string FormatLibassVersion(int version) {
	if (version < 0)
		return "unknown";
	// LIBASS_VERSION packs decimal version digits as two-digit hex fields
	// (e.g. 0.17.4 => 0x01704000).
	int const major = (version >> 28) & 0xF;
	int const minor = ((version >> 24) & 0xF) * 10 + ((version >> 20) & 0xF);
	int const micro = ((version >> 16) & 0xF) * 10 + ((version >> 12) & 0xF);
	return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(micro);
}

std::string FormatVersionDetail(int loaded_version) {
	return "headers=" + FormatLibassVersion(LIBASS_VERSION)
		+ ", dll=" + FormatLibassVersion(loaded_version);
}

std::string JoinCandidateNames(std::vector<std::string> const& names) {
	std::string result;
	for (size_t i = 0; i < names.size(); ++i) {
		if (i)
			result += ", ";
		result += names[i];
	}
	return result;
}

std::string FormatLoadError(std::string const& message) {
	auto const names = JoinCandidateNames(GetLibraryCandidates());
	if (message.empty())
		return "Could not load libass runtime (tried: " + names + ").";
	return "Could not load libass runtime (tried: " + names + "). " + message;
}

template <typename T>
void ResolveSymbol(agi::native::Library& library, T& out, char const *name) {
	out = library.ResolveSymbol<T>(name);
}

void ResolveSymbols(agi::native::Library& library, Api& loaded) {
#define AGI_LIBASS_FN(name) ResolveSymbol(library, loaded.name, #name);
#include "libass/libass_functions.inc"
#undef AGI_LIBASS_FN
}

void AddUniqueProbe(std::vector<std::string>& probes, std::string probe) {
	if (probe.empty())
		return;
	if (std::find(probes.begin(), probes.end(), probe) != probes.end())
		return;
	probes.push_back(std::move(probe));
}

// App-local probes only: every candidate name under every base directory, with
// no system fallback.
//
// Ordering is base-first, not candidate-first:
//   1. all candidates under each module base (versioned then unversioned)
//   2. all candidates under the host executable layout
//
// Nesting candidates outside bases would let host/versioned shadow
// module/unversioned and break "module bases first".
std::vector<std::string> CollectAppLocalProbes() {
	std::vector<std::string> probes;
	auto const options = agi::native::DefaultAppLocalLoadOptions(false);
	auto const candidates = GetLibraryCandidates();
	auto const bases = GetAppLocalSearchBases();

	// Phase 1: module directory (and any other explicit bases) — full candidate set.
	for (auto const& base : bases) {
		for (auto const& name : candidates) {
			for (auto const& probe : agi::native::BuildLibraryLoadProbes(name, base, options))
				AddUniqueProbe(probes, probe);
		}
	}

	// Phase 2: host executable directory (GetExecutableDirectory inside native_library).
	for (auto const& name : candidates) {
		for (auto const& probe : agi::native::BuildLibraryLoadProbes(name, options))
			AddUniqueProbe(probes, probe);
	}

	return probes;
}

bool TryInitializeFromLibrary(agi::native::Library library, RuntimeGlobals& g) {
	Api loaded;
	ResolveSymbols(library, loaded);

	// ass_library_version is a required symbol; always call after resolve.
	int const version = loaded.ass_library_version();
	std::string const path(library.GetLoadedPath());

	// Process-lifetime hold: detach the OS handle so Library's destructor does
	// not FreeLibrary/dlclose. Intentional one-time leak — libass is loaded at
	// most once per process.
	(void)library.ReleaseHandle();

	g.state.api = loaded;
	g.state.loaded_version = version;
	g.state.loaded_path = path;
	g.load_error.clear();
	g.load_complete = true;

	LOG_I(kLogTag) << "Loaded libass runtime from " << path
		<< " (" << FormatVersionDetail(version) << ")";
	return true;
}

void LoadRuntimeLocked(RuntimeGlobals& g) {
	if (g.load_complete)
		return;
	if (g.load_attempted) {
		throw agi::EnvironmentError(g.load_error.empty()
			? FormatLoadError({})
			: g.load_error);
	}
	g.load_attempted = true;

	std::string attempt_errors;
	auto record_error = [&](std::string const& label, std::string const& message) {
		if (!attempt_errors.empty())
			attempt_errors += "; ";
		attempt_errors += label;
		attempt_errors += ": ";
		attempt_errors += message;
	};

	// Pass 1: app-local only across all candidate names and all base directories.
	// This ensures a packaged unversioned libass.so is not shadowed by a system
	// libass.so.9 found mid-loop when system fallback is enabled per candidate.
	for (auto const& probe : CollectAppLocalProbes()) {
		try {
			if (TryInitializeFromLibrary(agi::native::Library::Load(probe), g))
				return;
		}
		catch (agi::EnvironmentError const& err) {
			record_error(probe, err.GetMessage());
		}
	}

#if !defined(_WIN32)
	// Pass 2 (Unix only): allow system/Homebrew/distro resolution for every
	// candidate name, after all app-local probes have failed.
	auto const system_options = agi::native::DefaultAppLocalLoadOptions(true);
	for (auto const& name : GetLibraryCandidates()) {
		try {
			if (TryInitializeFromLibrary(agi::native::Library::Load(name, system_options), g))
				return;
		}
		catch (agi::EnvironmentError const& err) {
			record_error(std::string("system:") + name, err.GetMessage());
		}
	}
#endif

	g.load_error = FormatLoadError(attempt_errors);
	LOG_W(kLogTag) << g.load_error;
	throw agi::EnvironmentError(g.load_error);
}

} // namespace

void EnsureLoaded() {
	auto& g = Globals();
	std::lock_guard<std::mutex> lock(g.mutex);
	LoadRuntimeLocked(g);
}

bool IsAvailable() noexcept {
	try {
		EnsureLoaded();
		return true;
	}
	catch (...) {
		return false;
	}
}

std::string GetLoadError() {
	auto& g = Globals();
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.load_error;
}

std::string GetLoadedLibrary() {
	auto& g = Globals();
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.state.loaded_path;
}

int GetLoadedVersion() noexcept {
	auto& g = Globals();
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.state.loaded_version;
}

Api const& GetApi() {
	EnsureLoaded();
	// load_complete implies api is fully published and remains read-only for
	// the process lifetime (module handle is never closed; globals never destroyed).
	return Globals().state.api;
}

}
