// Copyright (c) 2026

#include "native_library.h"

#include <libaegisub/exception.h>
#include <libaegisub/log.h>

#ifdef _WIN32
#include <libaegisub/charset_conv_win.h>
#include <windows.h>
#else
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif
#endif

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace agi { namespace native {

namespace {
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

	bool HasDirectorySeparator(std::string const& name) {
		return name.find('/') != std::string::npos || name.find('\\') != std::string::npos;
	}

	bool EndsWithCaseInsensitive(std::string const& value, std::string const& suffix) {
		if (suffix.size() > value.size()) return false;
		return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin(), [](char left, char right) {
			return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
		});
	}

	void AddCandidate(std::vector<std::string>& candidates, std::string const& candidate) {
		if (candidate.empty()) return;
		if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
			candidates.push_back(candidate);
	}

	std::vector<std::string> BuildCandidateVariations(std::string const& library_name) {
		std::vector<std::string> candidates;
		fs::path path(library_name);
		if (path.is_absolute()) {
			AddCandidate(candidates, library_name);
			return candidates;
		}

		bool has_separator = HasDirectorySeparator(library_name);
		std::string filename = path.filename().string();
		bool has_lib_prefix = filename.rfind("lib", 0) == 0;

#ifdef _WIN32
		AddCandidate(candidates, library_name);
		if (!EndsWithCaseInsensitive(library_name, ".dll") && !EndsWithCaseInsensitive(library_name, ".exe"))
			AddCandidate(candidates, library_name + ".dll");
#elif defined(__APPLE__)
		AddCandidate(candidates, library_name + ".dylib");
		if (!has_separator && !has_lib_prefix) AddCandidate(candidates, "lib" + library_name + ".dylib");
		AddCandidate(candidates, library_name);
		if (!has_separator && !has_lib_prefix) AddCandidate(candidates, "lib" + library_name);
#else
		bool has_so_name = library_name.find(".so") != std::string::npos;
		if (has_so_name) {
			AddCandidate(candidates, library_name);
			if (!has_separator && !has_lib_prefix) AddCandidate(candidates, "lib" + library_name);
			AddCandidate(candidates, library_name + ".so");
			if (!has_separator && !has_lib_prefix) AddCandidate(candidates, "lib" + library_name + ".so");
		}
		else {
			AddCandidate(candidates, library_name + ".so");
			if (!has_separator && !has_lib_prefix) AddCandidate(candidates, "lib" + library_name + ".so");
			AddCandidate(candidates, library_name);
			if (!has_separator && !has_lib_prefix) AddCandidate(candidates, "lib" + library_name);
		}
#endif

		return candidates;
	}

	std::string GetExecutableDirectory() {
#ifdef _WIN32
		std::wstring path(32768, L'\0');
		auto len = GetModuleFileNameW(nullptr, &path[0], static_cast<DWORD>(path.size()));
		if (!len) return {};
		path.resize(len);
		return fs::path(agi::charset::ConvertW(path)).parent_path().string();
#elif defined(__APPLE__)
		uint32_t size = 0;
		_NSGetExecutablePath(nullptr, &size);
		std::string path(size, '\0');
		if (_NSGetExecutablePath(path.data(), &size) != 0)
			return {};
		return fs::path(path.c_str()).parent_path().string();
#else
		std::vector<char> buffer(4096, '\0');
		auto len = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
		if (len <= 0) return {};
		buffer[static_cast<size_t>(len)] = '\0';
		return fs::path(buffer.data()).parent_path().string();
#endif
	}

	NativeHandle TryLoadLibrary(std::string const& candidate) {
#ifdef _WIN32
		return LoadLibraryW(agi::charset::ConvertW(candidate).c_str());
#else
		return dlopen(candidate.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
	}

	std::string GetLoadedLibraryPath(NativeHandle handle, std::string const& attempted_path) {
#ifdef _WIN32
		std::wstring path(32768, L'\0');
		auto len = GetModuleFileNameW(handle, &path[0], static_cast<DWORD>(path.size()));
		if (!len) return attempted_path;
		path.resize(len);
		return agi::charset::ConvertW(path);
#else
		return attempted_path;
#endif
	}
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

Library Library::Load(std::string const& library_name) {
	fs::path requested_path(library_name);
	auto candidates = BuildCandidateVariations(library_name);
	std::string app_dir = requested_path.is_absolute() ? std::string() : GetExecutableDirectory();
	std::string attempted;

	for (auto const& candidate : candidates) {
		auto try_candidate = [&](std::string const& probe) -> Library {
			if (!attempted.empty()) attempted += ", ";
			attempted += probe;
			auto native = TryLoadLibrary(probe);
			if (native)
				return Library(reinterpret_cast<void*>(native), library_name, GetLoadedLibraryPath(native, probe));
			auto reason = GetLoadFailureReason();
			if (!reason.empty()) attempted += " (" + reason + ")";
			return {};
		};

		if (!requested_path.is_absolute() && !app_dir.empty()) {
			auto app_probe = (fs::path(app_dir) / fs::path(candidate)).string();
			auto loaded = try_candidate(app_probe);
			if (loaded.handle) return loaded;
		}

		auto loaded = try_candidate(candidate);
		if (loaded.handle) return loaded;
	}

	throw agi::EnvironmentError("Could not load native library '" + library_name + "'. Tried: " + attempted);
}

void* Library::ResolveSymbolRaw(const char *symbol) {
	if (!handle)
		throw agi::EnvironmentError("Native library is not loaded.");

#ifdef _WIN32
	auto result = reinterpret_cast<void*>(GetProcAddress(ToNativeHandle(handle), symbol));
#else
	dlerror();
	auto result = dlsym(ToNativeHandle(handle), symbol);
#endif
	if (!result)
		throw agi::EnvironmentError(std::string("Failed to resolve native symbol ") + symbol + ": " + GetLoadFailureReason());

#ifndef _WIN32
	if (loaded_path.empty()) {
		Dl_info info{};
		if (dladdr(result, &info) && info.dli_fname)
			loaded_path = info.dli_fname;
	}
#endif

	return result;
}

CachedLibrary::CachedLibrary(std::string library_name, const char *display_name, const char *log_tag,
	InitializeFunction initialize, DetailFunction detail)
	: library_name(std::move(library_name))
	, display_name(display_name)
	, log_tag(log_tag)
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
		std::unique_ptr<Library> loaded(new Library(Library::Load(library_name)));
		if (initialize)
			initialize(*loaded);
		library = std::move(loaded);
		load_error.clear();
		load_complete = true;
		std::string message = std::string("Loaded ") + display_name + " from " + library->GetLoadedPath();
		if (detail) {
			auto suffix = detail();
			if (!suffix.empty())
				message += " (" + suffix + ")";
		}
		LOG_I(log_tag) << message;
		return *library;
	}
	catch (agi::EnvironmentError const& err) {
		load_error = err.GetMessage();
		if (detail) {
			auto suffix = detail();
			if (!suffix.empty())
				load_error += " (" + suffix + ")";
		}
		LOG_W(log_tag) << load_error;
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
	return library ? library->GetLoadedPath() : std::string();
}

void CachedLibrary::Reset() {
	std::lock_guard<std::mutex> lock(mutex);
	library.reset();
	load_error.clear();
	load_attempted = false;
	load_complete = false;
}

} }
