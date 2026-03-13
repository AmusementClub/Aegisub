// Copyright (c) 2026

#pragma once

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

} }

