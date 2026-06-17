// Copyright (c) 2026, MIR

#include <libaegisub/native_library.h>

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/string_utils.h>

#ifdef _WIN32
#include <libaegisub/charset_conv_win.h>
#include <windows.h>
#ifdef GetMessage
#undef GetMessage
#endif
#else
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace stdfs = std::filesystem;

namespace agi { namespace native {

namespace {
constexpr char kRuntimesSearchDir[] = "runtimes";

#ifdef _WIN32
	using NativeHandle = HMODULE;
#else
	using NativeHandle = void*;
#endif

	NativeHandle ToNativeHandle(void *handle) {
		return reinterpret_cast<NativeHandle>(handle);
	}

	std::string GetLoadFailureReason() {
#ifdef _WIN32
		auto error = GetLastError();
		if (!error)
			return "unknown error";
		LPWSTR buffer = nullptr;
		auto size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
		if (!size)
			return "unknown error";
		std::wstring message(buffer, size);
		LocalFree(buffer);
		while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'))
			message.pop_back();
		return agi::charset::ConvertW(message);
#else
		auto err = dlerror();
		return err ? err : "unknown error";
#endif
	}

	bool HasDirectorySeparator(std::string_view name) {
		return agi::util::strings::contains(name, '/') ||
			agi::util::strings::contains(name, '\\');
	}

	bool EndsWithCaseInsensitive(std::string_view value, std::string_view suffix) {
		if (suffix.size() > value.size()) return false;
		return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin(), [](char left, char right) {
			return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
		});
	}

	bool ContainsCaseInsensitive(std::string_view value, std::string_view needle) {
		if (needle.empty()) return true;
		if (needle.size() > value.size()) return false;
		for (size_t i = 0; i + needle.size() <= value.size(); ++i) {
			auto probe = value.substr(i, needle.size());
			if (std::equal(probe.begin(), probe.end(), needle.begin(), [](char left, char right) {
				return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
			}))
				return true;
		}
		return false;
	}

	void AddCandidate(std::vector<std::string>& candidates, std::string_view candidate) {
		if (candidate.empty()) return;
		if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
			candidates.emplace_back(candidate);
	}

	template <typename Range>
	std::string JoinCandidates(Range const& candidates) {
		std::string result;
		bool first = true;
		for (auto const& candidate : candidates) {
			if (!first)
				result += ", ";
			first = false;
			result += candidate;
		}
		return result;
	}

	std::string Concat(std::string_view left, std::string_view right) {
		std::string result;
		result.reserve(left.size() + right.size());
		result.append(left);
		result.append(right);
		return result;
	}

	std::string Concat(std::string_view left, std::string_view middle, std::string_view right) {
		std::string result;
		result.reserve(left.size() + middle.size() + right.size());
		result.append(left);
		result.append(middle);
		result.append(right);
		return result;
	}

	std::string GetExecutableDirectory() {
#ifdef _WIN32
		std::wstring path(32768, L'\0');
		auto len = GetModuleFileNameW(nullptr, &path[0], static_cast<DWORD>(path.size()));
		if (!len) return {};
		path.resize(len);
		return agi::fs::PathToString(stdfs::path(path).parent_path());
#elif defined(__APPLE__)
		uint32_t size = 0;
		_NSGetExecutablePath(nullptr, &size);
		std::string path(size, '\0');
		if (_NSGetExecutablePath(path.data(), &size) != 0)
			return {};
		return agi::fs::PathToString(agi::fs::PathFromString(path.c_str()).parent_path());
#else
		std::vector<char> buffer(4096, '\0');
		auto len = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
		if (len <= 0) return {};
		buffer[static_cast<size_t>(len)] = '\0';
		return agi::fs::PathToString(agi::fs::PathFromString(buffer.data()).parent_path());
#endif
	}

	NativeHandle TryLoadLibrary(std::string const& candidate) {
#ifdef _WIN32
		auto wide_candidate = agi::charset::ConvertW(candidate);
		auto path = stdfs::path(wide_candidate);
		if (path.has_parent_path()) {
			auto handle = LoadLibraryExW(
				wide_candidate.c_str(),
				nullptr,
				LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
			if (handle)
				return handle;
		}
		return LoadLibraryW(wide_candidate.c_str());
#else
		return dlopen(candidate.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
	}

	std::string GetLoadedLibraryPath(NativeHandle handle, std::string_view attempted_path) {
#ifdef _WIN32
		std::wstring path(32768, L'\0');
		auto len = GetModuleFileNameW(handle, &path[0], static_cast<DWORD>(path.size()));
		if (!len) return std::string(attempted_path);
		path.resize(len);
		return agi::charset::ConvertW(path);
#else
		return std::string(attempted_path);
#endif
	}

	bool IsExecutableRelativePathAllowed(stdfs::path const& path) {
		if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
			return false;

		for (auto const& component : path) {
			auto part = agi::fs::PathToGenericString(component);
			if (part.empty() || part == ".")
				continue;
			if (part == "..")
				return false;
		}

		return true;
	}

	bool LooksLikeDynamicLibrary(stdfs::path const& path) {
		if (!path.has_filename())
			return false;

		auto const filename = agi::fs::PathToString(path.filename());
#ifdef _WIN32
		return EndsWithCaseInsensitive(filename, ".dll");
#elif defined(__APPLE__)
		return EndsWithCaseInsensitive(filename, ".dylib");
#else
		return ContainsCaseInsensitive(filename, ".so");
#endif
	}
}

LibraryLoadOptions DefaultAppLocalLoadOptions(bool allow_system_fallback) {
	LibraryLoadOptions options;
	options.executable_relative_search_dirs.emplace_back(kRuntimesSearchDir);
	options.allow_system_fallback = allow_system_fallback;
	return options;
}

std::vector<std::string> BuildLibraryNameVariations(std::string_view library_name) {
	std::vector<std::string> candidates;
	stdfs::path path = agi::fs::PathFromString(std::string(library_name));
	if (path.is_absolute()) {
		AddCandidate(candidates, library_name);
		return candidates;
	}

	bool has_separator = HasDirectorySeparator(library_name);
	auto filename = agi::fs::PathToString(path.filename());
	bool has_lib_prefix = agi::util::strings::starts_with(filename, "lib");

#ifdef _WIN32
	AddCandidate(candidates, library_name);
	if (!EndsWithCaseInsensitive(library_name, ".dll") && !EndsWithCaseInsensitive(library_name, ".exe"))
		AddCandidate(candidates, Concat(library_name, ".dll"));
#elif defined(__APPLE__)
	if (EndsWithCaseInsensitive(filename, ".dylib")) {
		AddCandidate(candidates, library_name);
		if (!has_separator && !has_lib_prefix) AddCandidate(candidates, Concat("lib", library_name));
		return candidates;
	}

	// Match app-local probing expectations on macOS without duplicating explicit suffixes.
	AddCandidate(candidates, Concat(library_name, ".dylib"));
	if (!has_separator && !has_lib_prefix) AddCandidate(candidates, Concat("lib", library_name, ".dylib"));
	AddCandidate(candidates, library_name);
	if (!has_separator && !has_lib_prefix) AddCandidate(candidates, Concat("lib", library_name));
#else
	bool has_so_name = ContainsCaseInsensitive(filename, ".so");
	if (has_so_name) {
		AddCandidate(candidates, library_name);
		if (!has_separator && !has_lib_prefix) AddCandidate(candidates, Concat("lib", library_name));
		return candidates;
	}

	AddCandidate(candidates, Concat(library_name, ".so"));
	if (!has_separator && !has_lib_prefix) AddCandidate(candidates, Concat("lib", library_name, ".so"));
	AddCandidate(candidates, library_name);
	if (!has_separator && !has_lib_prefix) AddCandidate(candidates, Concat("lib", library_name));
#endif

	return candidates;
}

std::vector<std::string> BuildLibraryLoadProbes(
	std::string_view library_name,
	std::string_view executable_directory,
	LibraryLoadOptions const& options) {
	std::vector<std::string> probes;
	stdfs::path requested_path = agi::fs::PathFromString(std::string(library_name));
	if (requested_path.is_absolute()) {
		AddCandidate(probes, agi::fs::PathToString(requested_path));
		return probes;
	}

	if (requested_path.has_parent_path()) {
		auto normalized = requested_path.lexically_normal();
		if (!IsExecutableRelativePathAllowed(normalized))
			return probes;
		normalized.make_preferred();

		auto const candidates = BuildLibraryNameVariations(agi::fs::PathToString(normalized));
		stdfs::path exe_dir = agi::fs::PathFromString(std::string(executable_directory));
		for (auto const& candidate : candidates)
			AddCandidate(probes, agi::fs::PathToString(exe_dir / agi::fs::PathFromString(candidate)));
		return probes;
	}

	auto const candidates = BuildLibraryNameVariations(library_name);
	stdfs::path exe_dir = agi::fs::PathFromString(std::string(executable_directory));
	for (auto const& relative_dir : options.executable_relative_search_dirs) {
		if (relative_dir.empty())
			continue;

		stdfs::path relative_path(relative_dir);
		auto normalized = relative_path.lexically_normal();
		if (!IsExecutableRelativePathAllowed(normalized))
			continue;
		normalized.make_preferred();

		for (auto const& candidate : candidates)
			AddCandidate(probes, agi::fs::PathToString(exe_dir / normalized / agi::fs::PathFromString(candidate)));
	}

	for (auto const& candidate : candidates)
		AddCandidate(probes, agi::fs::PathToString(exe_dir / agi::fs::PathFromString(candidate)));

	if (options.allow_system_fallback) {
		for (auto const& candidate : candidates)
			AddCandidate(probes, candidate);
	}

	return probes;
}

std::vector<std::string> BuildLibraryLoadProbes(
	std::string_view library_name,
	LibraryLoadOptions const& options) {
	return BuildLibraryLoadProbes(library_name, GetExecutableDirectory(), options);
}

std::vector<std::string> EnumerateLibrariesInExecutableRelativeDirectory(
	std::string_view relative_directory,
	std::string_view executable_directory) {
	std::vector<std::string> libraries;
	stdfs::path relative_path = agi::fs::PathFromString(std::string(relative_directory));
	auto normalized = relative_path.lexically_normal();
	if (!IsExecutableRelativePathAllowed(normalized))
		return libraries;
	normalized.make_preferred();

	stdfs::path directory = agi::fs::PathFromString(std::string(executable_directory)) / normalized;
	std::error_code ec;
	if (!stdfs::exists(directory, ec) || !stdfs::is_directory(directory, ec))
		return libraries;

	for (auto const& entry : stdfs::directory_iterator(directory, ec)) {
		if (ec)
			break;
		if (!entry.is_regular_file(ec) || ec)
			continue;
		if (!LooksLikeDynamicLibrary(entry.path()))
			continue;
		libraries.emplace_back(agi::fs::PathToString(entry.path()));
	}

	std::sort(libraries.begin(), libraries.end());
	return libraries;
}

std::vector<std::string> EnumerateLibrariesInExecutableRelativeDirectory(std::string_view relative_directory) {
	return EnumerateLibrariesInExecutableRelativeDirectory(relative_directory, GetExecutableDirectory());
}

Library::Library(void *handle, std::string requested_name, std::string loaded_path)
	: handle(handle)
	, requested_name(std::move(requested_name))
	, loaded_path(std::move(loaded_path)) {
}

Library::~Library() {
	Reset();
}

Library::Library(Library&& other) noexcept
	: handle(other.handle)
	, requested_name(std::move(other.requested_name))
	, loaded_path(std::move(other.loaded_path)) {
	other.handle = nullptr;
}

Library& Library::operator=(Library&& other) noexcept {
	if (this != &other) {
		Reset();
		handle = other.handle;
		requested_name = std::move(other.requested_name);
		loaded_path = std::move(other.loaded_path);
		other.handle = nullptr;
	}
	return *this;
}

void Library::Reset() {
	if (!handle) return;
#ifdef _WIN32
	FreeLibrary(ToNativeHandle(handle));
#else
	dlclose(ToNativeHandle(handle));
#endif
	handle = nullptr;
	loaded_path.clear();
}

Library Library::Load(std::string_view library_name, LibraryLoadOptions const& options) {
	stdfs::path requested_path{std::string(library_name)};
	auto probes = BuildLibraryLoadProbes(library_name, requested_path.is_absolute() ? std::string() : GetExecutableDirectory(), options);
	if (agi::log::log) {
		if (!probes.empty())
			LOG_I("native/library") << "Probing native library '" << library_name << "' using: " << JoinCandidates(probes);
		else
			LOG_W("native/library") << "No valid probe paths generated for native library '" << library_name << "'";
	}
	std::string attempted;

	if (probes.empty())
		throw agi::EnvironmentError("Could not load native library '" + std::string(library_name) + "'. No valid load probes were generated.");

	for (auto const& probe : probes) {
		if (!attempted.empty()) attempted += ", ";
		attempted += probe;
		auto native = TryLoadLibrary(probe);
		if (native)
			return Library(reinterpret_cast<void*>(native), std::string(library_name), GetLoadedLibraryPath(native, probe));
		auto reason = GetLoadFailureReason();
		if (!reason.empty()) attempted += " (" + reason + ")";
	}

	throw agi::EnvironmentError("Could not load native library '" + std::string(library_name) + "'. Tried: " + attempted);
}

void* Library::ResolveSymbolRaw(std::string_view symbol) {
	if (!handle)
		throw agi::EnvironmentError("Native library is not loaded.");

#ifdef _WIN32
	auto result = reinterpret_cast<void*>(GetProcAddress(ToNativeHandle(handle), std::string(symbol).c_str()));
#else
	dlerror();
	auto result = dlsym(ToNativeHandle(handle), std::string(symbol).c_str());
#endif
	if (!result)
		throw agi::EnvironmentError(std::string("Failed to resolve native symbol ") + std::string(symbol) + ": " + GetLoadFailureReason());

#ifndef _WIN32
	if (loaded_path.empty()) {
		Dl_info info{};
		if (dladdr(result, &info) && info.dli_fname)
			loaded_path = info.dli_fname;
	}
#endif

	return result;
}

void* Library::TryResolveSymbolRaw(std::string_view symbol) noexcept {
	if (!handle)
		return nullptr;

#ifdef _WIN32
	return reinterpret_cast<void*>(GetProcAddress(ToNativeHandle(handle), std::string(symbol).c_str()));
#else
	dlerror();
	auto result = dlsym(ToNativeHandle(handle), std::string(symbol).c_str());
	return dlerror() ? nullptr : result;
#endif
}

void* Library::ReleaseHandle() {
	auto detached = handle;
	handle = nullptr;
	requested_name.clear();
	loaded_path.clear();
	return detached;
}

CachedLibrary::CachedLibrary(std::string_view library_name, std::string_view display_name, std::string_view log_tag,
	InitializeFunction initialize, DetailFunction detail, LibraryLoadOptions options)
	: library_name(library_name)
	, display_name(display_name)
	, log_tag(log_tag)
	, options(std::move(options))
	, initialize(std::move(initialize))
	, detail(std::move(detail)) {
}

Library& CachedLibrary::EnsureLoaded() {
	std::lock_guard<std::mutex> lock(mutex);
	if (load_complete)
		return *library;
	if (load_attempted)
		throw agi::EnvironmentError(load_error.empty() ? "Failed to load native library." : load_error);
	load_attempted = true;

	try {
		std::unique_ptr<Library> loaded(new Library(Library::Load(library_name, options)));
		if (initialize)
			initialize(*loaded);
		library = std::move(loaded);
		load_error.clear();
		load_complete = true;
		std::string message = std::string("Loaded ") + display_name + " from " + std::string(library->GetLoadedPath());
		if (detail) {
			auto suffix = detail();
			if (!suffix.empty())
				message += " (" + suffix + ")";
		}
		LOG_I(log_tag.c_str()) << message;
		return *library;
	}
	catch (agi::EnvironmentError const& err) {
		load_error = err.GetMessage();
		if (detail) {
			auto suffix = detail();
			if (!suffix.empty())
				load_error += " (" + suffix + ")";
		}
		LOG_W(log_tag.c_str()) << load_error;
		throw;
	}
}

bool CachedLibrary::IsAvailable() noexcept {
	try {
		EnsureLoaded();
		return true;
	}
	catch (...) {
		return false;
	}
}

std::string CachedLibrary::GetLoadError() const {
	std::lock_guard<std::mutex> lock(mutex);
	return load_error;
}

std::string CachedLibrary::GetLoadedLibrary() const {
	std::lock_guard<std::mutex> lock(mutex);
	return library ? std::string(library->GetLoadedPath()) : std::string();
}

void CachedLibrary::Reset() {
	std::lock_guard<std::mutex> lock(mutex);
	library.reset();
	load_error.clear();
	load_attempted = false;
	load_complete = false;
}

} }
