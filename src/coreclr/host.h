#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

namespace agi::coreclr {

std::filesystem::path GetCurrentExecutableDirectory();

struct HostOptions {
	std::filesystem::path nethost_path;
	std::filesystem::path runtime_config_path;
	std::filesystem::path dotnet_root;
};

struct HostProbe {
	std::filesystem::path hostfxr_path;
};

/// Resolve hostfxr and validate runtimeconfig/framework selection without
/// loading CoreCLR. This can safely choose between app-local and system roots.
HostProbe Probe(HostOptions const& options);

class Host final {
	class Impl;
	std::unique_ptr<Impl> impl;

public:
	explicit Host(HostOptions options);
	~Host();

	Host(Host const&) = delete;
	Host& operator=(Host const&) = delete;
	Host(Host&&) noexcept;
	Host& operator=(Host&&) noexcept;

	void* LoadUnmanagedEntryPoint(
		std::filesystem::path const& assembly_path,
		std::string_view assembly_qualified_type,
		std::string_view method_name) const;

	std::filesystem::path const& GetHostFxrPath() const;
};

} // namespace agi::coreclr
