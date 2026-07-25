#include "coreclr/adapter_bridge.h"
#include "coreclr/managed_plugin_activation.h"

#include <libaegisub/fs.h>
#include <libaegisub/io.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

#ifdef _WIN32
constexpr wchar_t kNetHostFileName[] = L"nethost.dll";
#elif defined(__APPLE__)
constexpr char kNetHostFileName[] = "libnethost.dylib";
#else
constexpr char kNetHostFileName[] = "libnethost.so";
#endif

int managed_callback_count = 0;
int macro_execute_callback_count = 0;
std::atomic_bool cancel_managed_invocation = false;
std::atomic_int progress_callback_count = 0;
std::atomic_int indeterminate_progress_callback_count = 0;
int host_service_execution_count = 0;
int emitted_plugin_event_count = 0;
int dispatched_plugin_event_count = 0;
std::string dependency_control_smoke_state_root;
std::string dependency_control_smoke_automation_root;

void AEGISUB_PLUGIN_BRIDGE_CALL LogManagedUtf8(uint8_t const* message, int32_t length) {
	if (!message || length < 0) return;
	auto text = std::string(reinterpret_cast<char const*>(message), static_cast<size_t>(length));
	if (text == "C# → C++ 反向回调成功")
		++managed_callback_count;
	if (text.starts_with("Executing aegisub.plugin-bridge.demo.runtime-info"))
		++macro_execute_callback_count;
	if (text.starts_with("Handled plugin event 'aegisub.bridge.smoke'"))
		++dispatched_plugin_event_count;
	std::cout << "managed_callback="
		<< text
		<< '\n';
}

struct PendingHostServiceCall {
	uint64_t plugin_handle = 0;
	int64_t invocation_token = 0;
	std::string service_id;
	std::string request_json;
	std::string result_json;
};

thread_local std::optional<PendingHostServiceCall> pending_host_service_call;

int32_t AEGISUB_PLUGIN_BRIDGE_CALL InvokeHostServiceUtf8(
	uint64_t plugin_handle,
	int64_t invocation_token,
	uint8_t const* service_id,
	int32_t service_id_length,
	uint8_t const* request_json,
	int32_t request_json_length,
	uint8_t* buffer,
	int32_t capacity,
	int32_t* payload_length) {
	using agi::coreclr::BridgeStatus;
	if (!plugin_handle || invocation_token < 0 || !service_id || service_id_length <= 0 ||
		!request_json || request_json_length <= 0 || capacity < 0 || !payload_length)
		return static_cast<int32_t>(BridgeStatus::InvalidArgument);
	auto service = std::string(
		reinterpret_cast<char const*>(service_id), static_cast<size_t>(service_id_length));
	auto request = std::string(
		reinterpret_cast<char const*>(request_json), static_cast<size_t>(request_json_length));
	auto& pending = pending_host_service_call;
	if (!pending || pending->plugin_handle != plugin_handle ||
		pending->invocation_token != invocation_token || pending->service_id != service ||
		pending->request_json != request) {
		pending.reset();
		std::string result;
		if (service == "aegisub.bridge.echo") {
			++host_service_execution_count;
			result = request;
		}
		else if (service == "aegisub.host.package-transaction.get-state-root" &&
			!dependency_control_smoke_state_root.empty()) {
			result = "{\"stateRoot\":\"" + dependency_control_smoke_state_root + "\"}";
		}
		else if (service == "aegisub.host.package-transaction.reconcile" &&
			!dependency_control_smoke_automation_root.empty()) {
			result = "{\"automationRoot\":\"" +
				dependency_control_smoke_automation_root +
				"\",\"recoveredTransactions\":0}";
		}
		else {
			return static_cast<int32_t>(BridgeStatus::NotFound);
		}
		pending = PendingHostServiceCall{
			plugin_handle,
			invocation_token,
			service,
			request,
			std::move(result)
		};
	}
	if (pending->result_json.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
		pending.reset();
		return static_cast<int32_t>(BridgeStatus::HostServiceFailed);
	}
	*payload_length = static_cast<int32_t>(pending->result_json.size());
	if (!buffer || capacity <= *payload_length)
		return static_cast<int32_t>(BridgeStatus::BufferTooSmall);
	std::memcpy(buffer, pending->result_json.data(), pending->result_json.size());
	buffer[pending->result_json.size()] = 0;
	pending.reset();
	return static_cast<int32_t>(BridgeStatus::Success);
}

int32_t AEGISUB_PLUGIN_BRIDGE_CALL EmitPluginEventUtf8(
	uint64_t plugin_handle,
	uint8_t const* event_id,
	int32_t event_id_length,
	uint8_t const* payload_json,
	int32_t payload_json_length) {
	using agi::coreclr::BridgeStatus;
	if (!plugin_handle || !event_id || event_id_length <= 0 ||
		!payload_json || payload_json_length <= 0)
		return static_cast<int32_t>(BridgeStatus::InvalidArgument);
	auto id = std::string(
		reinterpret_cast<char const*>(event_id), static_cast<size_t>(event_id_length));
	if (id == "aegisub.plugin.activated" && plugin_handle < (uint64_t{1} << 63))
		++emitted_plugin_event_count;
	return static_cast<int32_t>(BridgeStatus::Success);
}

int32_t AEGISUB_PLUGIN_BRIDGE_CALL IsCancellationRequested(int64_t) {
	return cancel_managed_invocation.load(std::memory_order_relaxed) ? 1 : 0;
}

int32_t AEGISUB_PLUGIN_BRIDGE_CALL ReportProgress(
	int64_t invocation_token,
	int64_t current,
	int64_t maximum,
	uint8_t const* message,
	int32_t message_length) {
	if (invocation_token <= 0 || current < 0 || maximum < 0 ||
		(maximum > 0 && current > maximum) || message_length < 0 ||
		(message_length > 0 && !message))
		return static_cast<int32_t>(agi::coreclr::BridgeStatus::InvalidArgument);
	++progress_callback_count;
	if (maximum == 0)
		++indeterminate_progress_callback_count;
	return static_cast<int32_t>(agi::coreclr::BridgeStatus::Success);
}

struct Arguments {
	std::filesystem::path managed_dir = AEGISUB_PLUGIN_BRIDGE_MANAGED_DIR;
	std::filesystem::path sample_dir = AEGISUB_PLUGIN_BRIDGE_SAMPLE_DIR;
	std::filesystem::path nethost_path;
	std::filesystem::path nativeaot_library;
	std::filesystem::path nativeaot_missing_export_library;
	std::filesystem::path nativeaot_incompatible_library;
	std::filesystem::path dotnet_root;
};

struct RemoveTreeOnExit {
	std::filesystem::path path;
	~RemoveTreeOnExit() {
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
};

void CopySamplePayload(
	std::filesystem::path const& source,
	std::filesystem::path const& destination) {
	std::error_code error;
	for (std::filesystem::directory_iterator iterator(source, error), end;
		!error && iterator != end; iterator.increment(error)) {
		if (!iterator->is_regular_file(error) || error) continue;
		std::filesystem::copy_file(
			iterator->path(),
			destination / iterator->path().filename(),
			std::filesystem::copy_options::overwrite_existing,
			error);
		if (error)
			throw std::runtime_error("Could not stage the managed plugin smoke payload");
	}
	if (error)
		throw std::runtime_error("Could not enumerate the managed plugin smoke payload");
}

std::set<std::filesystem::path> ListShadowRoots() {
	std::set<std::filesystem::path> result;
	auto root = std::filesystem::temp_directory_path() / "Aegisub" / "PluginBridge";
	std::error_code error;
	if (!std::filesystem::is_directory(root, error)) return result;
	for (std::filesystem::directory_iterator iterator(root, error), end;
		!error && iterator != end; iterator.increment(error)) {
		if (iterator->is_directory(error) && !error)
			result.insert(iterator->path());
	}
	return result;
}

std::optional<std::string> GetEnvironment(char const* name) {
	auto const* value = std::getenv(name);
	if (!value || !*value) return std::nullopt;
	return std::string(value);
}

void PrintUsage(char const* executable) {
	std::cout
		<< "Usage: " << executable << " [options]\n"
		<< "  --managed-dir PATH  Adapter build output\n"
		<< "  --sample-dir PATH   External extension build output\n"
		<< "  --nethost PATH      App-local nethost library\n"
		<< "  --dotnet-root PATH  Private dotnet_root (optional)\n"
		<< "Environment overrides: AEGISUB_NETHOST_PATH, AEGISUB_DOTNET_ROOT\n";
}

Arguments ParseArguments(int argc, char** argv) {
	Arguments result;
	auto executable_dir = std::filesystem::absolute(argv[0]).parent_path();
	result.nethost_path = executable_dir / kNetHostFileName;
	result.nativeaot_library = executable_dir /
		AEGISUB_DEPENDENCY_CONTROL_NATIVEAOT_FILENAME;
	result.nativeaot_missing_export_library = executable_dir /
		AEGISUB_NATIVEAOT_MISSING_EXPORT_FILENAME;
	result.nativeaot_incompatible_library = executable_dir /
		AEGISUB_NATIVEAOT_INCOMPATIBLE_FILENAME;

	if (auto value = GetEnvironment("AEGISUB_NETHOST_PATH"))
		result.nethost_path = std::filesystem::u8path(*value);
	if (auto value = GetEnvironment("AEGISUB_DOTNET_ROOT"))
		result.dotnet_root = std::filesystem::u8path(*value);

	for (int index = 1; index < argc; ++index) {
		std::string_view argument(argv[index]);
		if (argument == "--help" || argument == "-h") {
			PrintUsage(argv[0]);
			std::exit(0);
		}
		if (index + 1 >= argc)
			throw std::invalid_argument("Missing value after " + std::string(argument));
		auto value = std::filesystem::u8path(argv[++index]);
		if (argument == "--managed-dir") result.managed_dir = std::move(value);
		else if (argument == "--sample-dir") result.sample_dir = std::move(value);
		else if (argument == "--nethost") result.nethost_path = std::move(value);
		else if (argument == "--dotnet-root") result.dotnet_root = std::move(value);
		else throw std::invalid_argument("Unknown argument: " + std::string(argument));
	}
	return result;
}

void VerifyUnicodePathDiagnostics() {
	auto missing_nethost = std::filesystem::temp_directory_path() /
		std::filesystem::u8path(u8"Aegisub-路径-𠮷") / "missing-nethost.dll";
	auto expected_path = agi::fs::PathToString(missing_nethost);
	try {
		agi::coreclr::Probe({missing_nethost, {}, {}});
	}
	catch (std::exception const& error) {
		if (std::string_view(error.what()).find(expected_path) != std::string_view::npos)
			return;
		throw std::runtime_error(
			"CoreCLR Runtime path diagnostic did not preserve the UTF-8 path: " +
			std::string(error.what()));
	}
	throw std::runtime_error("CoreCLR Runtime unexpectedly accepted a missing Unicode nethost path");
}

void VerifyNativeAotBridge(
	Arguments const& arguments,
	agi::coreclr::NativeHostApi const& native_api) {
	using agi::coreclr::AdapterBridge;
	using agi::coreclr::NativeAdapterBridgeOptions;
	constexpr uint64_t native_handle = uint64_t{1} << 63;
	auto state_root = agi::fs::UniquePath(
		std::filesystem::temp_directory_path() /
			std::filesystem::path("Aegisub-nativeaot-smoke-%%%%%%%%"));
	RemoveTreeOnExit cleanup{state_root};
	auto automation_root = state_root / "automation";
	std::filesystem::create_directories(automation_root);
	dependency_control_smoke_state_root = agi::fs::PathToGenericString(state_root);
	dependency_control_smoke_automation_root =
		agi::fs::PathToGenericString(automation_root);

	auto expect_initialization_failure = [&](
		std::filesystem::path const& path,
		std::string_view expected_message,
		bool must_exist) {
		if (must_exist && !std::filesystem::is_regular_file(path))
			throw std::runtime_error("NativeAOT failure fixture was not deployed");
		try {
			AdapterBridge unexpected(NativeAdapterBridgeOptions{
				path, native_handle + 1, native_api});
		}
		catch (std::exception const& error) {
			if (std::string_view(error.what()).find(expected_message) !=
				std::string_view::npos)
				return;
			throw std::runtime_error(
				"NativeAOT initialization failed with an unexpected diagnostic: " +
				std::string(error.what()));
		}
		throw std::runtime_error(
			"An invalid NativeAOT plugin library unexpectedly initialized");
	};

	auto missing_library = arguments.nativeaot_library;
	missing_library += ".missing";
	expect_initialization_failure(missing_library, "Failed to load", false);
	expect_initialization_failure(
		arguments.nativeaot_missing_export_library, "Missing native export", true);
	expect_initialization_failure(
		arguments.nativeaot_incompatible_library, "incompatible ABI version", true);

	AdapterBridge native_bridge(NativeAdapterBridgeOptions{
		arguments.nativeaot_library, native_handle, native_api});
	auto runtime_info = native_bridge.GetRuntimeInfoUtf8();
	if (runtime_info.find("\"runtimeKind\":\"native\"") == std::string::npos)
		throw std::runtime_error("NativeAOT Bridge did not report its runtime kind");
	auto plugin = native_bridge.LoadNativePlugin(native_handle);
	if (plugin.handle != native_handle ||
		plugin.metadata_json.find("aegisub.dependency-control") == std::string::npos)
		throw std::runtime_error("NativeAOT Bridge returned unexpected plugin metadata");
	bool mismatched_handle_rejected = false;
	try {
		native_bridge.LoadNativePlugin(native_handle + 1);
	}
	catch (std::invalid_argument const&) {
		mismatched_handle_rejected = true;
	}
	if (!mismatched_handle_rejected)
		throw std::runtime_error("NativeAOT Bridge accepted a mismatched plugin handle");
	native_bridge.UnloadPlugin(native_handle);
	native_bridge.Shutdown();
	dependency_control_smoke_state_root.clear();
	dependency_control_smoke_automation_root.clear();

	std::cout << "nativeaot_high_handle=true\n";
	std::cout << "nativeaot_missing_library_rejected=true\n";
	std::cout << "nativeaot_missing_export_rejected=true\n";
	std::cout << "nativeaot_incompatible_table_rejected=true\n";
	std::cout << "nativeaot_coreclr_coexistence=true\n";
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
#endif
	try {
		VerifyUnicodePathDiagnostics();
		auto arguments = ParseArguments(argc, argv);
		auto runtime_config = arguments.managed_dir /
			"Aegisub.CoreClr.Adapter.runtimeconfig.json";
		auto assembly = arguments.managed_dir / "Aegisub.CoreClr.Adapter.dll";
		auto sample_assembly = arguments.sample_dir / "Aegisub.Managed.SampleExtension.dll";
		auto synthetic_stale_shadow = std::filesystem::temp_directory_path() /
			"Aegisub" / "PluginBridge" / "2147483646-1";
		std::filesystem::remove_all(synthetic_stale_shadow);
		std::filesystem::create_directories(synthetic_stale_shadow);

		std::cout << "nethost=" << agi::fs::PathToString(arguments.nethost_path) << '\n';
		std::cout << "runtimeconfig=" << agi::fs::PathToString(runtime_config) << '\n';
		if (!arguments.dotnet_root.empty())
			std::cout << "dotnet_root=" << agi::fs::PathToString(arguments.dotnet_root) << '\n';

		agi::coreclr::NativeHostApi native_api;
		native_api.log_utf8 = LogManagedUtf8;
		native_api.is_cancellation_requested = IsCancellationRequested;
		native_api.report_progress = ReportProgress;
		native_api.invoke_host_service_utf8 = InvokeHostServiceUtf8;
		native_api.emit_plugin_event_utf8 = EmitPluginEventUtf8;
		agi::coreclr::AdapterBridge bridge({
			{
				arguments.nethost_path,
				runtime_config,
				arguments.dotnet_root
			},
			assembly,
			native_api
		});
		if (std::filesystem::exists(synthetic_stale_shadow))
			throw std::runtime_error("Managed Adapter did not scavenge a dead-process shadow root");
		auto shadow_roots_after_startup = ListShadowRoots();
		auto runtime_info = bridge.GetRuntimeInfoUtf8();
		VerifyNativeAotBridge(arguments, native_api);

		bool failed_load_was_structured = false;
		try {
			bridge.LoadPlugin(
				sample_assembly,
				"Aegisub.Managed.SampleExtension.MissingModule");
		}
		catch (agi::coreclr::BridgeException const& error) {
			failed_load_was_structured =
				error.Status() == static_cast<int32_t>(
					agi::coreclr::BridgeStatus::ExtensionLoadFailed);
		}
		if (!failed_load_was_structured)
			throw std::runtime_error("Failed extension load did not return a structured status");

		auto extension = bridge.LoadPlugin(
			sample_assembly,
			"Aegisub.Managed.SampleExtension.SampleExtensionModule");
		if (extension.metadata_json.find("aegisub.plugin-bridge.demo") == std::string::npos)
			throw std::runtime_error("External extension metadata did not contain the expected ID");
		if (extension.metadata_json.find("\"entryModel\":\"plugin\"") == std::string::npos ||
			extension.metadata_json.find("\"kind\":\"automation\"") == std::string::npos ||
			extension.metadata_json.find("aegisub.plugin-bridge.demo.automation") == std::string::npos ||
			extension.metadata_json.find("\"kind\":\"settings\"") == std::string::npos)
			throw std::runtime_error(
				"External plugin metadata did not contain its Automation contribution");

		auto legacy_extension = bridge.LoadPlugin(
			sample_assembly,
			"Aegisub.Managed.SampleExtension.LegacySampleExtensionModule");
		if (legacy_extension.metadata_json.find(
				"\"entryModel\":\"legacyAutomationModule\"") == std::string::npos ||
			legacy_extension.metadata_json.find(
				"aegisub.plugin-bridge.legacy-demo.automation") == std::string::npos)
			throw std::runtime_error(
				"Legacy Automation module was not normalized to an Automation contribution");
		bool missing_event_handler_rejected = false;
		try {
			bridge.DispatchPluginEventUtf8(
				legacy_extension.handle,
				"aegisub.bridge.smoke",
				"{}");
		}
		catch (agi::coreclr::BridgeException const& error) {
			auto const& envelope = error.Error();
			missing_event_handler_rejected =
				error.Status() == static_cast<int32_t>(agi::coreclr::BridgeStatus::NotFound) &&
				envelope && envelope->code == "contract.event_handler_not_found";
		}
		if (!missing_event_handler_rejected)
			throw std::runtime_error("A plugin without an event handler accepted an event");
		bridge.UnloadPlugin(legacy_extension.handle);

		auto expect_contract_rejection = [&](char const* entry_type, std::string_view expected) {
			try {
				bridge.LoadPlugin(sample_assembly, entry_type);
			}
			catch (agi::coreclr::BridgeException const& error) {
				auto const& envelope = error.Error();
				if (error.Status() == static_cast<int32_t>(
						agi::coreclr::BridgeStatus::ExtensionLoadFailed) &&
					envelope && envelope->details.find(expected) != std::string::npos)
					return;
			}
			throw std::runtime_error(
				"Managed plugin contract validation did not reject " + std::string(entry_type));
		};
		expect_contract_rejection(
			"Aegisub.Managed.SampleExtension.DuplicateContributionPlugin",
			"Duplicate C# plugin contribution ID");
		expect_contract_rejection(
			"Aegisub.Managed.SampleExtension.WrongContributionKindPlugin",
			"declares kind 'Command'");
		expect_contract_rejection(
			"Aegisub.Managed.SampleExtension.MissingHostServicePlugin",
			"aegisub.bridge.missing-service");
		bridge.DispatchPluginEventUtf8(
			extension.handle,
			"aegisub.bridge.smoke",
			"{\"source\":\"native-smoke\"}");
		if (dispatched_plugin_event_count != 1)
			throw std::runtime_error("Native-to-managed plugin event was not dispatched exactly once");
		bool malformed_event_rejected = false;
		try {
			bridge.DispatchPluginEventUtf8(
				extension.handle,
				"aegisub.bridge.smoke",
				"{");
		}
		catch (agi::coreclr::BridgeException const& error) {
			auto const& envelope = error.Error();
			malformed_event_rejected =
				error.Status() == static_cast<int32_t>(
					agi::coreclr::BridgeStatus::EventDispatchFailed) &&
				envelope && envelope->code == "host.invalid_event_payload" &&
				envelope->category == agi::coreclr::BridgeErrorCategory::Host;
		}
		if (!malformed_event_rejected)
			throw std::runtime_error("Malformed native plugin event JSON was not rejected");

		auto macro_result = bridge.InvokeContributionUtf8(
			extension.handle,
			"aegisub.plugin-bridge.demo.automation",
			"aegisub.plugin-bridge.demo.runtime-info",
			"{\"invocationToken\":1,\"subtitles\":null}");
		bool missing_contribution_rejected = false;
		try {
			bridge.InvokeContributionUtf8(
				extension.handle,
				"aegisub.plugin-bridge.demo.missing",
				"aegisub.plugin-bridge.demo.runtime-info",
				"{\"invocationToken\":5,\"subtitles\":null}");
		}
		catch (agi::coreclr::BridgeException const& error) {
			auto const& envelope = error.Error();
			missing_contribution_rejected =
				error.Status() == static_cast<int32_t>(agi::coreclr::BridgeStatus::NotFound) &&
				envelope && envelope->code == "contract.contribution_not_found";
		}
		if (!missing_contribution_rejected)
			throw std::runtime_error("A missing contribution target was not rejected");
		bool unsupported_operation_rejected = false;
		try {
			bridge.InvokeContributionUtf8(
				extension.handle,
				"aegisub.plugin-bridge.demo.settings",
				"settings.open",
				"{}");
		}
		catch (agi::coreclr::BridgeException const& error) {
			auto const& envelope = error.Error();
			unsupported_operation_rejected =
				error.Status() == static_cast<int32_t>(agi::coreclr::BridgeStatus::NotFound) &&
				envelope && envelope->code == "contract.operation_not_found";
		}
		if (!unsupported_operation_rejected)
			throw std::runtime_error("A descriptor-only contribution accepted an operation");
		if (progress_callback_count != 2 || indeterminate_progress_callback_count != 1)
			throw std::runtime_error("Managed Macro did not report the expected progress sequence");

		bool structured_extension_failure = false;
		try {
			bridge.InvokeMacroUtf8(
				extension.handle,
				"aegisub.plugin-bridge.demo.trim-selected-line-endings",
				"{\"invocationToken\":3,\"subtitles\":null}");
		}
		catch (agi::coreclr::BridgeException const& error) {
			auto const& envelope = error.Error();
			structured_extension_failure =
				error.Status() == static_cast<int32_t>(
					agi::coreclr::BridgeStatus::InvocationFailed) &&
				envelope &&
				envelope->code == "aegisub.subtitle.required" &&
				envelope->category == agi::coreclr::BridgeErrorCategory::Extension &&
				!envelope->retryable &&
				envelope->exception_type.ends_with("AutomationFailureException") &&
				envelope->details.find("AutomationFailureException") != std::string::npos &&
				std::string_view(error.what()).find("AutomationFailureException") == std::string_view::npos;
		}
		if (!structured_extension_failure)
			throw std::runtime_error("An extension-reported failure did not produce a structured Bridge error");

		bool structured_host_failure = false;
		try {
			bridge.InvokeMacroUtf8(
				extension.handle,
				"aegisub.plugin-bridge.demo.runtime-info",
				"{");
		}
		catch (agi::coreclr::BridgeException const& error) {
			auto const& envelope = error.Error();
			structured_host_failure = envelope &&
				envelope->code == "host.invalid_context" &&
				envelope->category == agi::coreclr::BridgeErrorCategory::Host;
		}
		if (!structured_host_failure)
			throw std::runtime_error("Invalid native context did not produce a structured host error");

		bool incomplete_host_failure = false;
		try {
			bridge.InvokeMacroUtf8(
				extension.handle,
				"aegisub.plugin-bridge.demo.runtime-info",
				"{\"invocationToken\":4}");
		}
		catch (agi::coreclr::BridgeException const& error) {
			auto const& envelope = error.Error();
			incomplete_host_failure = envelope &&
				envelope->code == "host.invalid_context" &&
				envelope->category == agi::coreclr::BridgeErrorCategory::Host;
		}
		if (!incomplete_host_failure)
			throw std::runtime_error("Incomplete native context did not produce a structured host error");

		cancel_managed_invocation.store(true, std::memory_order_relaxed);
		bool cancellation_propagated = false;
		try {
			bridge.InvokeMacroUtf8(
				extension.handle,
				"aegisub.plugin-bridge.demo.runtime-info",
				"{\"invocationToken\":2,\"subtitles\":null}");
		}
		catch (agi::coreclr::InvocationCancelled const& error) {
			auto const& envelope = error.Error();
			cancellation_propagated = envelope &&
				envelope->code == "invocation.cancelled" &&
				envelope->category == agi::coreclr::BridgeErrorCategory::Invocation &&
				envelope->retryable;
		}
		cancel_managed_invocation.store(false, std::memory_order_relaxed);
		if (!cancellation_propagated)
			throw std::runtime_error("Managed Macro cancellation was not propagated as a typed Bridge result");

		// The original package must remain replaceable while the managed assembly is
		// loaded. Only the shadow copy is allowed to be locked by CoreCLR.
		auto lock_probe = sample_assembly;
		lock_probe += ".lock-probe";
		std::filesystem::copy_file(
			sample_assembly, lock_probe, std::filesystem::copy_options::overwrite_existing);
		std::filesystem::copy_file(
			lock_probe, sample_assembly, std::filesystem::copy_options::overwrite_existing);
		std::filesystem::remove(lock_probe);
		bridge.UnloadPlugin(extension.handle);
		bool stale_handle_rejected = false;
		try {
			bridge.InvokeMacroUtf8(
				extension.handle,
				"aegisub.plugin-bridge.demo.runtime-info",
				"{\"invocationToken\":2,\"subtitles\":null}");
		}
		catch (agi::coreclr::BridgeException const& error) {
			auto const& envelope = error.Error();
			stale_handle_rejected =
				error.Status() == static_cast<int32_t>(agi::coreclr::BridgeStatus::InvalidHandle) &&
				envelope &&
				envelope->code == "bridge.invalid_handle" &&
				envelope->category == agi::coreclr::BridgeErrorCategory::Bridge;
		}
		if (!stale_handle_rejected)
			throw std::runtime_error("An unloaded extension handle was not rejected as invalid");

		constexpr int reload_cycles = 30;
		for (int cycle = 0; cycle < reload_cycles; ++cycle) {
			auto reloaded = bridge.LoadPlugin(
				sample_assembly,
				"Aegisub.Managed.SampleExtension.SampleExtensionModule");
			bridge.UnloadPlugin(reloaded.handle);
		}
		if (managed_callback_count != 2)
			throw std::runtime_error("Expected one CoreCLR and one NativeAOT callback, got " +
				std::to_string(managed_callback_count));
		if (macro_execute_callback_count != 1)
			throw std::runtime_error("Expected the two-pass ABI call to execute the Macro exactly once, got " +
				std::to_string(macro_execute_callback_count));
		if (host_service_execution_count != reload_cycles + 1)
			throw std::runtime_error(
				"Host-service two-pass calls executed more than once per plugin activation");
		if (emitted_plugin_event_count != reload_cycles + 1)
			throw std::runtime_error("Managed-to-native plugin activation events were lost");

		std::cout << "hostfxr=" << agi::fs::PathToString(bridge.GetHostFxrPath()) << '\n';
		std::cout << "managed=" << runtime_info << '\n';
		std::cout << "macro=" << macro_result << '\n';
		std::cout << "reload_cycles=" << reload_cycles << '\n';
		std::cout << "original_dll_replaceable=true\n";
		std::cout << "stale_handle_rejected=true\n";
		std::cout << "progress_callbacks=" << progress_callback_count << '\n';
		std::cout << "cancellation_propagated=true\n";
		std::cout << "structured_extension_failure=true\n";
		std::cout << "structured_host_failure=true\n";
		std::cout << "managed_plugin_contributions=true\n";
		std::cout << "legacy_automation_module=true\n";
		std::cout << "contribution_validation=true\n";
		std::cout << "generic_contribution_invocation=true\n";
		std::cout << "host_service_calls=" << host_service_execution_count << '\n';
		std::cout << "managed_to_native_events=" << emitted_plugin_event_count << '\n';
		std::cout << "native_to_managed_events=" << dispatched_plugin_event_count << '\n';
		std::cout << "generic_transport_errors=true\n";
		bridge.Shutdown();
		if (ListShadowRoots() != shadow_roots_after_startup)
			throw std::runtime_error(
				"Bridge shutdown left a current-process shadow root after a failed load");
		std::cout << "failed_load_shadow_cleanup=true\n";
		std::cout << "stale_shadow_scavenging=true\n";
		std::cout << "unicode_path_diagnostics=true\n";

		// CoreCLR remains process-global after host shutdown. A fresh Bridge must
		// accept hostfxr's positive "already initialized" success status and be
		// able to initialize and shut down the managed Adapter again.
		{
			agi::coreclr::AdapterBridge restarted_bridge({
				{
					arguments.nethost_path,
					runtime_config,
					arguments.dotnet_root
				},
				assembly,
				native_api
			});
			auto restarted_runtime_info = restarted_bridge.GetRuntimeInfoUtf8();
			if (restarted_runtime_info.find("Aegisub.CoreClr.Adapter") == std::string::npos)
				throw std::runtime_error("Restarted Bridge did not return managed Runtime info");

			auto store_root = agi::fs::UniquePath(
				std::filesystem::temp_directory_path() /
				std::filesystem::path("Aegisub-managed-plugin-smoke-%%%%%%%%"));
			RemoveTreeOnExit cleanup{store_root};
			agi::coreclr::ManagedPluginActivationStore store(store_root);
			auto staged = store.CreateStagingDirectory(
				"aegisub.plugin-bridge.demo", "0.2.0");
			CopySamplePayload(arguments.sample_dir, staged);
			{
				agi::io::Save manifest(
					staged / agi::coreclr::kManagedPluginManifestFilename);
				manifest.Get()
					<< R"({"manifestVersion":2,"id":"aegisub.plugin-bridge.demo","name":"Plugin Bridge Demo","version":"0.2.0","contractsVersion":"0.1.0","runtime":{"kind":"coreclr","entryAssembly":"Aegisub.Managed.SampleExtension.dll","entryType":"Aegisub.Managed.SampleExtension.SampleExtensionModule"},"contributions":[]})";
				manifest.Close();
			}
			store.PublishStagedVersion(
				"aegisub.plugin-bridge.demo", "0.2.0", staged);
			store.Activate("aegisub.plugin-bridge.demo", "0.2.0");

			auto failing_staged = store.CreateStagingDirectory(
				"aegisub.plugin-bridge.demo", "0.3.0");
			CopySamplePayload(arguments.sample_dir, failing_staged);
			{
				agi::io::Save manifest(
					failing_staged / agi::coreclr::kManagedPluginManifestFilename);
				manifest.Get()
					<< R"({"manifestVersion":2,"id":"aegisub.plugin-bridge.demo","name":"Plugin Bridge Demo","version":"0.3.0","contractsVersion":"0.1.0","runtime":{"kind":"coreclr","entryAssembly":"Aegisub.Managed.SampleExtension.dll","entryType":"Aegisub.Managed.SampleExtension.MissingModule"},"contributions":[]})";
				manifest.Close();
			}
			store.PublishStagedVersion(
				"aegisub.plugin-bridge.demo", "0.3.0", failing_staged);
			store.Activate("aegisub.plugin-bridge.demo", "0.3.0");
			store.SetPinnedVersion("aegisub.plugin-bridge.demo", "0.3.0");
			auto discovery = store.Discover();
			if (!discovery.diagnostics.empty() || discovery.candidates.size() != 1)
				throw std::runtime_error("Versioned local plugin discovery failed");
			auto const& candidate = discovery.candidates.front();
			if (candidate.version != "0.3.0" || !candidate.fallback ||
				candidate.fallback->version != "0.2.0")
				throw std::runtime_error("Versioned local plugin fallback was not discovered");
			bool active_activation_failed = false;
			try {
				auto unexpected = restarted_bridge.LoadPlugin(
					candidate.manifest_path.parent_path() /
						"Aegisub.Managed.SampleExtension.dll",
					"Aegisub.Managed.SampleExtension.MissingModule");
				restarted_bridge.UnloadPlugin(unexpected.handle);
			}
			catch (agi::coreclr::BridgeException const&) {
				active_activation_failed = true;
			}
			if (!active_activation_failed)
				throw std::runtime_error(
					"Invalid active managed plugin version unexpectedly loaded");
			auto versioned = restarted_bridge.LoadPlugin(
				candidate.fallback->manifest_path.parent_path() /
					"Aegisub.Managed.SampleExtension.dll",
				"Aegisub.Managed.SampleExtension.SampleExtensionModule");
			if (!store.CommitAutomaticRollback(
					candidate.plugin_id,
					candidate.generation,
					candidate.version,
					candidate.fallback->version))
				throw std::runtime_error(
					"Managed plugin automatic rollback was not committed");
			auto rolled_back = store.ReadState(candidate.plugin_id);
			if (rolled_back.active_version != "0.2.0" ||
				rolled_back.previous_version ||
				rolled_back.pinned_version ||
				!rolled_back.last_failed_version ||
				*rolled_back.last_failed_version != "0.3.0")
				throw std::runtime_error(
					"Managed plugin automatic rollback state is inconsistent");
			restarted_bridge.UnloadPlugin(versioned.handle);
			restarted_bridge.Shutdown();
		}
		std::cout << "bridge_restart=true\n";
		std::cout << "versioned_local_activation=true\n";
		std::cout << "versioned_managed_rollback=true\n";
		return 0;
	}
	catch (std::exception const& error) {
		std::cerr << "Plugin Bridge unavailable: " << error.what() << '\n';
		return 2;
	}
}
