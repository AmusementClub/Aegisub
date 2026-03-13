// Copyright (c) 2026

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace agi { namespace native {

class Library {
	void *handle = nullptr;
	std::string requested_name;
	std::string loaded_path;

	void* ResolveSymbolRaw(const char *symbol);

public:
	Library() = default;
	Library(void *handle, std::string requested_name, std::string loaded_path);
	~Library();

	Library(Library const&) = delete;
	Library& operator=(Library const&) = delete;
	Library(Library&& other) noexcept;
	Library& operator=(Library&& other) noexcept;

	static Library Load(std::string const& library_name);

	template <typename T>
	T ResolveSymbol(const char *symbol) {
		return reinterpret_cast<T>(ResolveSymbolRaw(symbol));
	}

	std::string const& GetRequestedName() const { return requested_name; }
	std::string const& GetLoadedPath() const { return loaded_path; }
	void Reset();
};

class CachedLibrary {
public:
	typedef std::function<void(Library&)> InitializeFunction;
	typedef std::function<std::string()> DetailFunction;

private:
	std::string library_name;
	const char *display_name;
	const char *log_tag;
	InitializeFunction initialize;
	DetailFunction detail;
	mutable std::mutex mutex;
	std::unique_ptr<Library> library;
	std::string load_error;
	bool load_attempted = false;
	bool load_complete = false;

public:
	CachedLibrary(std::string library_name, const char *display_name, const char *log_tag,
		InitializeFunction initialize = InitializeFunction(), DetailFunction detail = DetailFunction());

	Library& EnsureLoaded();
	bool IsAvailable() noexcept;
	std::string GetLoadError() const;
	std::string GetLoadedLibrary() const;
	void Reset();
};

} }
