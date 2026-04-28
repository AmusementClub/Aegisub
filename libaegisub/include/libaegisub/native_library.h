// Copyright (c) 2026, MIR

#pragma once

#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace agi { namespace native {

struct LibraryLoadOptions {
	std::vector<std::string> executable_relative_search_dirs;
	bool allow_system_fallback = false;

	bool operator==(LibraryLoadOptions const&) const = default;
};

LibraryLoadOptions DefaultAppLocalLoadOptions(bool allow_system_fallback = false);
std::vector<std::string> BuildLibraryNameVariations(std::string_view library_name);
std::vector<std::string> BuildLibraryLoadProbes(
	std::string_view library_name,
	std::string_view executable_directory,
	LibraryLoadOptions const& options);
std::vector<std::string> BuildLibraryLoadProbes(
	std::string_view library_name,
	LibraryLoadOptions const& options);
std::vector<std::string> EnumerateLibrariesInExecutableRelativeDirectory(
	std::string_view relative_directory,
	std::string_view executable_directory);
std::vector<std::string> EnumerateLibrariesInExecutableRelativeDirectory(std::string_view relative_directory);

class Library {
	void *handle = nullptr;
	std::string requested_name;
	std::string loaded_path;

	void* ResolveSymbolRaw(std::string_view symbol);
	void* TryResolveSymbolRaw(std::string_view symbol) noexcept;

public:
	Library() = default;
	Library(void *handle, std::string requested_name, std::string loaded_path);
	~Library();

	Library(Library const&) = delete;
	Library& operator=(Library const&) = delete;
	Library(Library&& other) noexcept;
	Library& operator=(Library&& other) noexcept;

	static Library Load(std::string_view library_name, LibraryLoadOptions const& options = DefaultAppLocalLoadOptions());

	template <typename T>
	T ResolveSymbol(std::string_view symbol) {
		return reinterpret_cast<T>(ResolveSymbolRaw(symbol));
	}

	template <typename T>
	T TryResolveSymbol(std::string_view symbol) noexcept {
		return reinterpret_cast<T>(TryResolveSymbolRaw(symbol));
	}

	std::string_view GetRequestedName() const { return requested_name; }
	std::string_view GetLoadedPath() const { return loaded_path; }
	void* ReleaseHandle();
	void Reset();
};

class CachedLibrary {
public:
	typedef std::function<void(Library&)> InitializeFunction;
	typedef std::function<std::string()> DetailFunction;

private:
	std::string library_name;
	std::string display_name;
	std::string log_tag;
	LibraryLoadOptions options;
	InitializeFunction initialize;
	DetailFunction detail;
	mutable std::mutex mutex;
	std::unique_ptr<Library> library;
	std::string load_error;
	bool load_attempted = false;
	bool load_complete = false;

public:
	CachedLibrary(std::string_view library_name, std::string_view display_name, std::string_view log_tag,
		InitializeFunction initialize = InitializeFunction(),
		DetailFunction detail = DetailFunction(),
		LibraryLoadOptions options = DefaultAppLocalLoadOptions());

	Library& EnsureLoaded();
	bool IsAvailable() noexcept;
	std::string GetLoadError() const;
	std::string GetLoadedLibrary() const;
	void Reset();
};

} }
