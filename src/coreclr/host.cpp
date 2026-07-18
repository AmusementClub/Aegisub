#include "host.h"

#include "native_library.h"

#include <libaegisub/fs.h>

#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>

#ifdef _WIN32
#include <windows.h>
#else
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif
#endif

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace agi::coreclr {
namespace {

std::string StatusHex(int32_t status) {
	std::ostringstream stream;
	stream << "0x" << std::hex << std::uppercase << static_cast<uint32_t>(status);
	return stream.str();
}

bool IsHostFxrSuccess(int32_t status) noexcept {
	// hostfxr also returns positive success codes when compatible hosting
	// components or a compatible CoreCLR are already initialized.
	return status >= 0;
}

std::string HostTextToUtf8(char_t const* text) {
	if (!text) return {};
#ifdef _WIN32
	auto const* wide = reinterpret_cast<wchar_t const*>(text);
	auto required = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
	if (required <= 1) return {};
	std::string result(static_cast<size_t>(required), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), required, nullptr, nullptr);
	result.pop_back();
	return result;
#else
	return text;
#endif
}

std::basic_string<char_t> ToHostString(std::filesystem::path const& path) {
#ifdef _WIN32
	return path.native();
#else
	return agi::fs::PathToString(path);
#endif
}

std::basic_string<char_t> ToHostString(std::string_view text) {
#ifdef _WIN32
	if (text.empty()) return {};
	auto required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
		static_cast<int>(text.size()), nullptr, 0);
	if (required <= 0)
		throw std::runtime_error("Bridge type or method name is not valid UTF-8");
	std::wstring result(static_cast<size_t>(required), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
		static_cast<int>(text.size()), result.data(), required);
	return result;
#else
	return std::string(text);
#endif
}

void HOSTFXR_CALLTYPE HostFxrErrorWriter(char_t const* message) {
	std::cerr << "[hostfxr] " << HostTextToUtf8(message) << '\n';
}

using get_hostfxr_path_fn = int32_t(NETHOST_CALLTYPE *)(
	char_t*, size_t*, get_hostfxr_parameters const*);

std::filesystem::path ResolveHostFxrPath(
	NativeLibrary const& nethost,
	std::filesystem::path const& dotnet_root) {
	auto get_hostfxr_path = nethost.Resolve<get_hostfxr_path_fn>("get_hostfxr_path");
	auto dotnet_root_text = ToHostString(dotnet_root);
	get_hostfxr_parameters parameters{
		sizeof(get_hostfxr_parameters),
		nullptr,
		dotnet_root.empty() ? nullptr : dotnet_root_text.c_str()
	};

	std::vector<char_t> buffer(4096);
	auto buffer_size = buffer.size();
	auto status = get_hostfxr_path(buffer.data(), &buffer_size,
		dotnet_root.empty() ? nullptr : &parameters);
	if (status != 0 && buffer_size > buffer.size()) {
		buffer.resize(buffer_size);
		status = get_hostfxr_path(buffer.data(), &buffer_size,
			dotnet_root.empty() ? nullptr : &parameters);
	}
	if (status != 0)
		throw std::runtime_error("get_hostfxr_path failed with status " + StatusHex(status));

	return std::filesystem::path(buffer.data());
}

void ValidateHostOptions(HostOptions const& options) {
	if (options.nethost_path.empty())
		throw std::invalid_argument("nethost path is empty");
	if (!std::filesystem::is_regular_file(options.nethost_path))
		throw std::runtime_error(
			"nethost was not found: " + agi::fs::PathToString(options.nethost_path));
	if (!std::filesystem::is_regular_file(options.runtime_config_path))
		throw std::runtime_error(
			"runtimeconfig was not found: " + agi::fs::PathToString(options.runtime_config_path));
	if (!options.dotnet_root.empty() && !std::filesystem::is_directory(options.dotnet_root))
		throw std::runtime_error(
			"dotnet root was not found: " + agi::fs::PathToString(options.dotnet_root));
}

} // namespace

std::filesystem::path GetCurrentExecutableDirectory() {
#ifdef _WIN32
	std::wstring path(32768, L'\0');
	auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	if (!length || length >= path.size())
		throw std::runtime_error("Could not resolve the executable path");
	path.resize(length);
	return std::filesystem::path(path).parent_path();
#elif defined(__APPLE__)
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::vector<char> path(size, '\0');
	if (_NSGetExecutablePath(path.data(), &size) != 0)
		throw std::runtime_error("Could not resolve the executable path");
	return std::filesystem::weakly_canonical(path.data()).parent_path();
#else
	std::vector<char> path(4096, '\0');
	auto length = readlink("/proc/self/exe", path.data(), path.size() - 1);
	if (length <= 0)
		throw std::runtime_error("Could not resolve the executable path");
	path[static_cast<size_t>(length)] = '\0';
	return std::filesystem::path(path.data()).parent_path();
#endif
}

HostProbe Probe(HostOptions const& options) {
	ValidateHostOptions(options);

	NativeLibrary nethost;
	nethost.Load(std::filesystem::absolute(options.nethost_path));
	auto hostfxr_path = ResolveHostFxrPath(nethost, options.dotnet_root);
	NativeLibrary hostfxr;
	hostfxr.Load(hostfxr_path);

	auto initialize = hostfxr.Resolve<hostfxr_initialize_for_runtime_config_fn>(
		"hostfxr_initialize_for_runtime_config");
	auto close = hostfxr.Resolve<hostfxr_close_fn>("hostfxr_close");
	auto set_error_writer = hostfxr.Resolve<hostfxr_set_error_writer_fn>(
		"hostfxr_set_error_writer");
	auto previous_error_writer = set_error_writer(HostFxrErrorWriter);
	hostfxr_handle context = nullptr;
	try {
		auto runtime_config = ToHostString(
			std::filesystem::absolute(options.runtime_config_path));
		auto dotnet_root = ToHostString(options.dotnet_root);
		hostfxr_initialize_parameters parameters{
			sizeof(hostfxr_initialize_parameters),
			nullptr,
			options.dotnet_root.empty() ? nullptr : dotnet_root.c_str()
		};
		auto status = initialize(
			runtime_config.c_str(),
			options.dotnet_root.empty() ? nullptr : &parameters,
			&context);
		if (!IsHostFxrSuccess(status) || !context)
			throw std::runtime_error(
				"hostfxr runtime probe failed with status " + StatusHex(status));

		// Runtime-config initialization resolves frameworks and host policy. It
		// does not load CoreCLR until a runtime delegate is requested.
		close(context);
		context = nullptr;
		set_error_writer(previous_error_writer);
		return {std::move(hostfxr_path)};
	}
	catch (...) {
		if (context)
			close(context);
		set_error_writer(previous_error_writer);
		throw;
	}
}

class Host::Impl final {
public:
	NativeLibrary nethost;
	NativeLibrary hostfxr;
	std::filesystem::path hostfxr_path;
	load_assembly_and_get_function_pointer_fn load_assembly = nullptr;

	void Initialize(HostOptions const& options) {
		ValidateHostOptions(options);

		nethost.Load(std::filesystem::absolute(options.nethost_path));
		hostfxr_path = ResolveHostFxrPath(nethost, options.dotnet_root);
		hostfxr.Load(hostfxr_path);

		auto initialize = hostfxr.Resolve<hostfxr_initialize_for_runtime_config_fn>(
			"hostfxr_initialize_for_runtime_config");
		auto get_delegate = hostfxr.Resolve<hostfxr_get_runtime_delegate_fn>(
			"hostfxr_get_runtime_delegate");
		auto close = hostfxr.Resolve<hostfxr_close_fn>("hostfxr_close");
		auto set_error_writer = hostfxr.Resolve<hostfxr_set_error_writer_fn>(
			"hostfxr_set_error_writer");
		auto previous_error_writer = set_error_writer(HostFxrErrorWriter);
		hostfxr_handle context = nullptr;

		try {
			auto runtime_config = ToHostString(
				std::filesystem::absolute(options.runtime_config_path));
			auto dotnet_root = ToHostString(options.dotnet_root);
			hostfxr_initialize_parameters parameters{
				sizeof(hostfxr_initialize_parameters),
				nullptr,
				options.dotnet_root.empty() ? nullptr : dotnet_root.c_str()
			};
			auto status = initialize(runtime_config.c_str(),
				options.dotnet_root.empty() ? nullptr : &parameters, &context);
			if (!IsHostFxrSuccess(status) || !context)
				throw std::runtime_error(
					"hostfxr initialization failed with status " + StatusHex(status));

			void* delegate = nullptr;
			status = get_delegate(
				context, hdt_load_assembly_and_get_function_pointer, &delegate);
			if (status != 0 || !delegate)
				throw std::runtime_error(
					"hostfxr delegate lookup failed with status " + StatusHex(status));

			// The runtime delegate remains valid after the hostfxr context is
			// closed. The error writer is thread-local, so both operations must
			// be paired on this initialization thread.
			close(context);
			context = nullptr;
			set_error_writer(previous_error_writer);
			load_assembly = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(delegate);
		}
		catch (...) {
			if (context)
				close(context);
			set_error_writer(previous_error_writer);
			throw;
		}
	}
};

Host::Host(HostOptions options)
:	impl(std::make_unique<Impl>()) {
	impl->Initialize(options);
}

Host::~Host() = default;
Host::Host(Host&&) noexcept = default;
Host& Host::operator=(Host&&) noexcept = default;

void* Host::LoadUnmanagedEntryPoint(
	std::filesystem::path const& assembly_path,
	std::string_view assembly_qualified_type,
	std::string_view method_name) const {
	if (!impl || !impl->load_assembly)
		throw std::logic_error(".NET Runtime host is not initialized");
	if (!std::filesystem::is_regular_file(assembly_path))
		throw std::runtime_error(
			"managed assembly was not found: " + agi::fs::PathToString(assembly_path));

	auto assembly = ToHostString(std::filesystem::absolute(assembly_path));
	auto type = ToHostString(assembly_qualified_type);
	auto method = ToHostString(method_name);
	void* entry_point = nullptr;
	auto status = impl->load_assembly(
		assembly.c_str(), type.c_str(), method.c_str(),
		UNMANAGEDCALLERSONLY_METHOD, nullptr, &entry_point);
	if (status != 0 || !entry_point)
		throw std::runtime_error("managed entry point lookup failed with status " + StatusHex(status));
	return entry_point;
}

std::filesystem::path const& Host::GetHostFxrPath() const {
	if (!impl) throw std::logic_error(".NET Runtime host is not initialized");
	return impl->hostfxr_path;
}

} // namespace agi::coreclr
