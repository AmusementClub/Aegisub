#include "dependency_control_host.h"

#include "dependency_control_transaction.h"

#include "auto4_base.h"
#include "options.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Automation4 {
namespace {

constexpr std::string_view kBeginService =
	"aegisub.dependency-control.begin-transaction";
constexpr std::string_view kCommitService =
	"aegisub.dependency-control.commit-transaction";
constexpr std::string_view kAbortService =
	"aegisub.dependency-control.abort-transaction";
constexpr std::string_view kStateRootService =
	"aegisub.dependency-control.get-state-root";
constexpr std::string_view kReconcileService =
	"aegisub.dependency-control.reconcile-transactions";

std::optional<std::wstring> ReadWideEnvironment(wchar_t const* name) {
#ifdef _WIN32
	auto length = GetEnvironmentVariableW(name, nullptr, 0);
	if (!length)
		return std::nullopt;
	std::wstring value(length, L'\0');
	auto written = GetEnvironmentVariableW(name, value.data(), length);
	if (!written || written >= length)
		throw std::runtime_error("Could not read DependencyControl environment setting");
	value.resize(written);
	return value;
#else
	(void)name;
	return std::nullopt;
#endif
}

std::optional<std::string> ReadEnvironment(char const* name) {
	auto const* value = std::getenv(name);
	if (!value || !*value)
		return std::nullopt;
	return std::string(value);
}

agi::fs::path ResolveAutomationRoot() {
#ifdef _WIN32
	if (auto value = ReadWideEnvironment(
		L"AEGISUB_DEPENDENCY_CONTROL_AUTOMATION_ROOT"))
		return agi::fs::path{*value};
#else
	if (auto value = ReadEnvironment("AEGISUB_DEPENDENCY_CONTROL_AUTOMATION_ROOT"))
		return agi::fs::PathFromString(*value);
#endif
	if (!config::path)
		throw std::runtime_error(
			"DependencyControl Automation root is unavailable before path initialization");
	return config::path->Decode("?user/automation");
}

agi::fs::path ResolveStateRoot() {
#ifdef _WIN32
	if (auto value = ReadWideEnvironment(
		L"AEGISUB_DEPENDENCY_CONTROL_STATE_ROOT"))
		return agi::fs::path{*value};
#else
	if (auto value = ReadEnvironment("AEGISUB_DEPENDENCY_CONTROL_STATE_ROOT"))
		return agi::fs::PathFromString(*value);
#endif
	if (!config::path)
		throw std::runtime_error(
			"DependencyControl state root is unavailable before path initialization");
	return config::path->Decode("?user/dependency-control");
}

agi::fs::path ResolveLegacyConfigRoot() {
#ifdef _WIN32
	if (auto value = ReadWideEnvironment(
		L"AEGISUB_DEPENDENCY_CONTROL_LEGACY_CONFIG_ROOT"))
		return agi::fs::path{*value};
#else
	if (auto value = ReadEnvironment("AEGISUB_DEPENDENCY_CONTROL_LEGACY_CONFIG_ROOT"))
		return agi::fs::PathFromString(*value);
#endif
	if (!config::path)
		throw std::runtime_error(
			"DependencyControl legacy configuration root is unavailable before path initialization");
	return config::path->Decode("?user/config");
}

size_t ReadCommitFailurePoint() {
	std::optional<std::string> value;
#ifdef _WIN32
	if (auto wide = ReadWideEnvironment(
		L"AEGISUB_DEPENDENCY_CONTROL_TEST_FAIL_COMMIT_AFTER")) {
		if (!wide->empty() &&
			std::all_of(wide->begin(), wide->end(), [](wchar_t character) {
				return character >= L'0' && character <= L'9';
			}))
			value = std::string(wide->begin(), wide->end());
	}
#else
	value = ReadEnvironment("AEGISUB_DEPENDENCY_CONTROL_TEST_FAIL_COMMIT_AFTER");
#endif
	if (!value)
		return 0;
	size_t result = 0;
	auto [end, error] = std::from_chars(
		value->data(), value->data() + value->size(), result);
	if (error != std::errc{} || end != value->data() + value->size() || !result)
		throw std::runtime_error(
			"DependencyControl test commit failure point is invalid");
	return result;
}

std::string Serialize(json::Object const& value) {
	std::ostringstream stream;
	agi::JsonWriter::Write(value, stream);
	return std::move(stream).str();
}

json::Object ParseObject(std::string const& value, std::string const& description) {
	std::istringstream stream(value);
	json::UnknownElement root;
	try {
		json::Reader::Read(root, stream);
		return std::move(static_cast<json::Object&>(root));
	}
	catch (std::exception const& error) {
		throw std::runtime_error(
			"DependencyControl " + description + " must be a JSON object: " + error.what());
	}
}

std::string RequireString(
	json::Object const& object,
	std::string const& key,
	size_t maximum_length) {
	auto it = object.find(key);
	if (it == object.end())
		throw std::runtime_error(
			"DependencyControl host request requires field '" + key + "'");
	std::string value;
	try {
		value = static_cast<json::String const&>(it->second);
	}
	catch (...) {
		throw std::runtime_error(
			"DependencyControl host request field '" + key + "' must be a string");
	}
	if (value.empty() || value.size() > maximum_length)
		throw std::runtime_error(
			"DependencyControl host request field '" + key + "' is invalid");
	return value;
}

std::string OptionalString(
	json::Object const& object,
	std::string const& key,
	size_t maximum_length) {
	auto it = object.find(key);
	if (it == object.end())
		return {};
	std::string value;
	try {
		value = static_cast<json::String const&>(it->second);
	}
	catch (...) {
		throw std::runtime_error(
			"DependencyControl host request field '" + key + "' must be a string");
	}
	if (value.size() > maximum_length)
		throw std::runtime_error(
			"DependencyControl host request field '" + key + "' is too long");
	return value;
}

bool OptionalBool(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end())
		return false;
	try {
		return static_cast<json::Boolean const&>(it->second);
	}
	catch (...) {
		throw std::runtime_error(
			"DependencyControl host request field '" + key + "' must be boolean");
	}
}

json::Array const& RequireArray(
	json::Object const& object,
	std::string const& key) {
	auto it = object.find(key);
	if (it == object.end())
		throw std::runtime_error(
			"DependencyControl host request requires array '" + key + "'");
	try {
		return static_cast<json::Array const&>(it->second);
	}
	catch (...) {
		throw std::runtime_error(
			"DependencyControl host request field '" + key + "' must be an array");
	}
}

struct HostState {
	std::mutex mutex;
	std::shared_ptr<DependencyControlTransactionStore> store;
};

HostState& State() {
	static HostState state;
	return state;
}

std::shared_ptr<DependencyControlTransactionStore> Store() {
	auto& state = State();
	std::lock_guard<std::mutex> lock(state.mutex);
	if (!state.store) {
		auto failure_point = ReadCommitFailurePoint();
		DependencyControlTransactionStore::RescanCallback rescan;
		if (config::global_scripts) {
			rescan = [] {
				agi::dispatch::Main().Async([] {
					if (config::global_scripts)
						config::global_scripts->ReloadAsync();
				});
			};
		}
		DependencyControlTransactionStore::CommitFaultCallback fault;
		if (failure_point) {
			fault = [failure_point](size_t completed) {
				if (completed == failure_point)
					throw std::runtime_error(
						"Injected DependencyControl commit failure");
			};
		}
		state.store = std::make_shared<DependencyControlTransactionStore>(
			ResolveAutomationRoot(), std::move(rescan), std::move(fault));
	}
	return state.store;
}

std::string Begin(uint64_t plugin_handle) {
	auto transaction = Store()->Begin(plugin_handle);
	json::Object response;
	response["schemaVersion"] = json::Integer(1);
	response["transactionId"] = transaction.transaction_id;
	response["stagingRoot"] = agi::fs::PathToString(transaction.staging_root);
	response["automationRoot"] = agi::fs::PathToString(transaction.automation_root);
	return Serialize(response);
}

std::string Commit(uint64_t plugin_handle, std::string const& request_json) {
	auto request = ParseObject(request_json, "commit request");
	auto transaction_id = RequireString(request, "transactionId", 128);
	auto const& items = RequireArray(request, "files");
	if (items.size() > 2048)
		throw std::runtime_error("DependencyControl commit request exceeds the file limit");
	std::vector<DependencyControlTransactionFile> files;
	files.reserve(items.size());
	for (auto const& item : items) {
		json::Object const* object = nullptr;
		try {
			object = &static_cast<json::Object const&>(item);
		}
		catch (...) {
			throw std::runtime_error(
				"DependencyControl commit file entries must be objects");
		}
		files.push_back({
			OptionalString(*object, "stagedName", 128),
			RequireString(*object, "target", 4096),
			OptionalBool(*object, "delete")
		});
	}
	auto result = Store()->Commit(plugin_handle, transaction_id, files);
	json::Object response;
	response["schemaVersion"] = json::Integer(1);
	response["committed"] = true;
	response["fileCount"] = json::Integer(static_cast<int64_t>(result.file_count));
	response["rescanScheduled"] = result.rescan_requested;
	return Serialize(response);
}

std::string Abort(uint64_t plugin_handle, std::string const& request_json) {
	auto request = ParseObject(request_json, "abort request");
	auto transaction_id = RequireString(request, "transactionId", 128);
	Store()->Abort(plugin_handle, transaction_id);
	json::Object response;
	response["schemaVersion"] = json::Integer(1);
	response["aborted"] = true;
	return Serialize(response);
}

std::string GetStateRoot() {
	json::Object response;
	response["schemaVersion"] = json::Integer(1);
	response["stateRoot"] = agi::fs::PathToString(ResolveStateRoot());
	response["legacyConfigRoot"] = agi::fs::PathToString(ResolveLegacyConfigRoot());
	return Serialize(response);
}

std::string ReconcileTransactions() {
	auto store = Store();
	json::Object response;
	response["schemaVersion"] = json::Integer(1);
	response["automationRoot"] = agi::fs::PathToString(store->AutomationRoot());
	response["recoveredTransactions"] = json::Integer(
		static_cast<int64_t>(store->RecoveredTransactionCount()));
	return Serialize(response);
}

} // namespace

std::optional<std::string> InvokeDependencyControlHostService(
	uint64_t plugin_handle,
	std::string const& service_id,
	std::string const& request_json) {
	if (service_id == kBeginService)
		return Begin(plugin_handle);
	if (service_id == kCommitService)
		return Commit(plugin_handle, request_json);
	if (service_id == kAbortService)
		return Abort(plugin_handle, request_json);
	if (service_id == kStateRootService)
		return GetStateRoot();
	if (service_id == kReconcileService)
		return ReconcileTransactions();
	return std::nullopt;
}

void AbortDependencyControlTransactions(uint64_t plugin_handle) noexcept {
	std::shared_ptr<DependencyControlTransactionStore> store;
	{
		auto& state = State();
		std::lock_guard<std::mutex> lock(state.mutex);
		store = state.store;
	}
	if (store)
		store->AbortPlugin(plugin_handle);
}

void ShutdownDependencyControlHost() noexcept {
	std::shared_ptr<DependencyControlTransactionStore> store;
	{
		auto& state = State();
		std::lock_guard<std::mutex> lock(state.mutex);
		store = std::move(state.store);
	}
	if (store)
		store->AbortAll();
}

} // namespace Automation4
