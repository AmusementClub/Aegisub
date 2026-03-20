// Copyright (c) 2026

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace agi { namespace native {

std::vector<std::string> BuildLibraryNameVariations(std::string_view library_name);

class Library {
	void *handle = nullptr;
	std::string requested_name;
	std::string loaded_path;

	void* ResolveSymbolRaw(std::string_view symbol);

public:
	Library() = default;
	Library(void *handle, std::string requested_name, std::string loaded_path);
	~Library();

	Library(Library const&) = delete;
	Library& operator=(Library const&) = delete;
	Library(Library&& other) noexcept;
	Library& operator=(Library&& other) noexcept;

	static Library Load(std::string_view library_name);

	template <typename T>
	T ResolveSymbol(std::string_view symbol) {
		return reinterpret_cast<T>(ResolveSymbolRaw(symbol));
	}

	std::string_view GetRequestedName() const { return requested_name; }
	std::string_view GetLoadedPath() const { return loaded_path; }
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
	InitializeFunction initialize;
	DetailFunction detail;
	mutable std::mutex mutex;
	std::unique_ptr<Library> library;
	std::string load_error;
	bool load_attempted = false;
	bool load_complete = false;

public:
	CachedLibrary(std::string_view library_name, std::string_view display_name, std::string_view log_tag,
		InitializeFunction initialize = InitializeFunction(), DetailFunction detail = DetailFunction());

	Library& EnsureLoaded();
	bool IsAvailable() noexcept;
	std::string GetLoadError() const;
	std::string GetLoadedLibrary() const;
	void Reset();
};

} }
