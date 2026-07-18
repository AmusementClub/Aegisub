#include "native_library.h"

#ifdef _WIN32
#else
#include <dlfcn.h>
#endif

#include <stdexcept>
#include <string>

namespace agi::coreclr {

NativeLibrary::~NativeLibrary() {
	Reset();
}

NativeLibrary::NativeLibrary(NativeLibrary&& other) noexcept {
	*this = std::move(other);
}

NativeLibrary& NativeLibrary::operator=(NativeLibrary&& other) noexcept {
	if (this == &other) return *this;
	Reset();
	handle = other.handle;
	other.handle = nullptr;
	return *this;
}

void NativeLibrary::Load(std::filesystem::path const& path) {
	Reset();
#ifdef _WIN32
	handle = LoadLibraryExW(
		path.c_str(), nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!handle)
		throw std::runtime_error(
			"Failed to load " + agi::fs::PathToString(path) +
			" (Win32 error " + std::to_string(GetLastError()) + ")");
#else
	handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		auto const* error = dlerror();
		throw std::runtime_error(
			"Failed to load " + agi::fs::PathToString(path) + ": " +
			(error ? error : "unknown dynamic loader error"));
	}
#endif
}

void* NativeLibrary::ResolveRaw(char const* name) const {
	if (!handle) throw std::logic_error("Native library is not loaded");
#ifdef _WIN32
	auto symbol = reinterpret_cast<void*>(GetProcAddress(handle, name));
#else
	auto symbol = dlsym(handle, name);
#endif
	if (!symbol)
		throw std::runtime_error(std::string("Missing native export: ") + name);
	return symbol;
}

void NativeLibrary::Reset() noexcept {
	if (!handle) return;
#ifdef _WIN32
	FreeLibrary(handle);
#else
	dlclose(handle);
#endif
	handle = nullptr;
}

} // namespace agi::coreclr
