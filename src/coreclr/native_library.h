#pragma once

#include <libaegisub/fs.h>

#include <filesystem>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef CreateDirectory
#endif

namespace agi::coreclr {

class NativeLibrary final {
#ifdef _WIN32
	HMODULE handle = nullptr;
#else
	void* handle = nullptr;
#endif

	void* ResolveRaw(char const* name) const;

public:
	NativeLibrary() = default;
	~NativeLibrary();

	NativeLibrary(NativeLibrary const&) = delete;
	NativeLibrary& operator=(NativeLibrary const&) = delete;
	NativeLibrary(NativeLibrary&& other) noexcept;
	NativeLibrary& operator=(NativeLibrary&& other) noexcept;

	void Load(std::filesystem::path const& path);

	template<typename Function>
	Function Resolve(char const* name) const {
		return reinterpret_cast<Function>(ResolveRaw(name));
	}

	/// Keep a native plugin resident until process exit. Native modules may own
	/// runtime state which is not collectible like a CoreCLR AssemblyLoadContext.
	void Detach() noexcept { handle = nullptr; }

	void Reset() noexcept;
};

} // namespace agi::coreclr
