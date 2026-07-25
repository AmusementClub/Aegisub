#include "package_transaction.h"

#include <libaegisub/fs.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#undef CreateDirectory
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Automation4 {
namespace {

constexpr size_t kMaximumFiles = 2048;
constexpr size_t kMaximumTargetBytes = 4096;
constexpr size_t kMaximumStagedNameBytes = 128;
constexpr std::string_view kJournalName = ".native-journal-v1";
constexpr std::string_view kJournalHeader =
	"Aegisub.Package.Transaction/1";
constexpr std::string_view kLegacyJournalHeader =
	"Aegisub.DependencyControl.Transaction/1";

std::runtime_error FileSystemError(
	std::string const& operation,
	agi::fs::path const& path,
	std::error_code const& error) {
	return std::runtime_error(
		operation + " '" + agi::fs::PathToString(path) + "': " + error.message());
}

bool IsLinkLike(agi::fs::path const& path) {
	std::error_code error;
	auto status = std::filesystem::symlink_status(path, error);
	if (error == std::errc::no_such_file_or_directory)
		return false;
	if (error)
		throw FileSystemError("Could not inspect Package transaction path", path, error);
	if (std::filesystem::is_symlink(status))
		return true;
#ifdef _WIN32
	auto attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		auto native_error = GetLastError();
		if (native_error == ERROR_FILE_NOT_FOUND || native_error == ERROR_PATH_NOT_FOUND)
			return false;
		throw FileSystemError(
			"Could not inspect Package transaction path",
			path,
			std::error_code(static_cast<int>(native_error), std::system_category()));
	}
	return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
	return false;
#endif
}

bool Exists(agi::fs::path const& path) {
	std::error_code error;
	auto result = std::filesystem::exists(std::filesystem::symlink_status(path, error));
	if (error == std::errc::no_such_file_or_directory)
		return false;
	if (error)
		throw FileSystemError("Could not inspect Package transaction path", path, error);
	return result;
}

void RequireSafeDirectory(agi::fs::path const& path) {
	if (IsLinkLike(path))
		throw std::runtime_error(
			"Package transaction refuses a linked or reparse-point directory: " +
			agi::fs::PathToString(path));
	std::error_code error;
	if (!std::filesystem::is_directory(path, error)) {
		if (error)
			throw FileSystemError("Could not inspect Package transaction directory", path, error);
		throw std::runtime_error(
			"Package transaction path is not a directory: " + agi::fs::PathToString(path));
	}
}

void CreateSafeDirectory(agi::fs::path const& path) {
	std::error_code error;
	std::filesystem::create_directory(path, error);
	if (error)
		throw FileSystemError("Could not create Package transaction directory", path, error);
	RequireSafeDirectory(path);
}

void CreateSafeDirectories(
	agi::fs::path const& trusted_root,
	agi::fs::path const& relative) {
	RequireSafeDirectory(trusted_root);
	auto current = trusted_root;
	for (auto const& component : relative) {
		current /= component;
		if (Exists(current))
			RequireSafeDirectory(current);
		else
			CreateSafeDirectory(current);
	}
}

agi::fs::path ParseTarget(std::string const& value) {
	if (value.empty() || value.size() > kMaximumTargetBytes ||
		value.front() == '/' || value.front() == '\\' ||
		value.find('\0') != std::string::npos || value.find(':') != std::string::npos)
		throw std::runtime_error("Package transaction target is invalid");
	for (unsigned char character : value) {
		if (character < 0x20 || character == 0x7f)
			throw std::runtime_error(
				"Package transaction target contains control characters");
	}

	std::string portable = value;
	std::replace(portable.begin(), portable.end(), '\\', '/');
	for (size_t begin = 0; begin <= portable.size();) {
		auto end = portable.find('/', begin);
		auto component = std::string_view(portable).substr(
			begin,
			end == std::string::npos ? portable.size() - begin : end - begin);
		if (component.empty() || component == "." || component == ".." ||
			component.size() > 255)
			throw std::runtime_error(
				"Package transaction target contains an invalid path component");
#ifdef _WIN32
		if (component.back() == '.' || component.back() == ' ' ||
			component.find_first_of("<>\"|?*") != std::string_view::npos)
			throw std::runtime_error(
				"Package transaction target is not a portable Windows path");
		auto device_end = component.find('.');
		auto device = std::string(component.substr(0, device_end));
		std::transform(device.begin(), device.end(), device.begin(), [](unsigned char value) {
			return value >= 'a' && value <= 'z'
				? static_cast<char>(value - 'a' + 'A')
				: static_cast<char>(value);
		});
		bool numbered_device = device.size() == 4 &&
			(device.starts_with("COM") || device.starts_with("LPT")) &&
			device[3] >= '1' && device[3] <= '9';
		if (device == "CON" || device == "PRN" || device == "AUX" ||
			device == "NUL" || numbered_device)
			throw std::runtime_error(
				"Package transaction target uses a reserved Windows filename");
#endif
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}

	auto path = agi::fs::PathFromString(portable);
	if (path.empty() || path.is_absolute() || path.has_root_name() ||
		path.has_root_directory() || path.filename().empty())
		throw std::runtime_error("Package transaction target must be relative");
	return path;
}

agi::fs::path ParseStagedName(std::string const& value) {
	if (value.empty() || value.size() > kMaximumStagedNameBytes ||
		value == "." || value == "..")
		throw std::runtime_error("Package transaction staged filename is invalid");
	for (unsigned char character : value) {
		bool allowed = (character >= 'a' && character <= 'z') ||
			(character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') ||
			character == '.' || character == '_' || character == '-';
		if (!allowed)
			throw std::runtime_error("Package transaction staged filename is invalid");
	}
	return agi::fs::PathFromString(value);
}

void RequireRegularFile(agi::fs::path const& path, std::string const& description) {
	if (IsLinkLike(path))
		throw std::runtime_error(
			"Package transaction refuses a linked or reparse-point " + description);
	std::error_code error;
	if (!std::filesystem::is_regular_file(path, error)) {
		if (error)
			throw FileSystemError("Could not inspect Package transaction file", path, error);
		throw std::runtime_error("Package transaction " + description + " is not a regular file");
	}
}

void Rename(agi::fs::path const& from, agi::fs::path const& to) {
	std::error_code error;
	std::filesystem::rename(from, to, error);
	if (error)
		throw std::runtime_error(
			"Could not move Package transaction file from '" +
			agi::fs::PathToString(from) + "' to '" + agi::fs::PathToString(to) +
			"': " + error.message());
}

void RemoveFileIfPresent(agi::fs::path const& path) {
	std::error_code error;
	std::filesystem::remove(path, error);
	if (error && error != std::errc::no_such_file_or_directory)
		throw FileSystemError("Could not remove Package transaction file", path, error);
}

void RemoveTreeNoThrow(agi::fs::path const& path) noexcept {
	std::error_code error;
	std::filesystem::remove_all(path, error);
}

struct Transaction {
	uint64_t plugin_handle = 0;
	std::string id;
	agi::fs::path root;
};

struct PreparedFile {
	agi::fs::path staged;
	agi::fs::path relative_target;
	agi::fs::path target;
	bool remove = false;
};

struct AppliedFile {
	agi::fs::path target;
	agi::fs::path backup;
	bool installed = false;
	bool backed_up = false;
};

void FlushFileToDisk(agi::fs::path const& path) {
#ifdef _WIN32
	auto handle = CreateFileW(
		path.c_str(),
		GENERIC_WRITE,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (handle == INVALID_HANDLE_VALUE)
		throw FileSystemError(
			"Could not open Package transaction journal",
			path,
			std::error_code(static_cast<int>(GetLastError()), std::system_category()));
	auto close = [&] { CloseHandle(handle); };
	if (!FlushFileBuffers(handle)) {
		auto error = GetLastError();
		close();
		throw FileSystemError(
			"Could not flush Package transaction journal",
			path,
			std::error_code(static_cast<int>(error), std::system_category()));
	}
	close();
#else
	auto descriptor = open(path.c_str(), O_RDONLY);
	if (descriptor < 0)
		throw FileSystemError(
			"Could not open Package transaction journal",
			path,
			std::error_code(errno, std::generic_category()));
	if (fsync(descriptor) != 0) {
		auto error = errno;
		close(descriptor);
		throw FileSystemError(
			"Could not flush Package transaction journal",
			path,
			std::error_code(error, std::generic_category()));
	}
	close(descriptor);
#endif
}

void WriteCommitJournal(
	agi::fs::path const& transaction_root,
	std::vector<PreparedFile> const& files) {
	auto path = transaction_root / agi::fs::PathFromString(std::string(kJournalName));
	if (Exists(path))
		throw std::runtime_error(
			"Package transaction contains the reserved journal path");
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	if (!stream)
		throw FileSystemError(
			"Could not create Package transaction journal",
			path,
			std::make_error_code(std::errc::io_error));
	stream << kJournalHeader << '\n' << files.size() << '\n';
	for (auto const& file : files) {
		stream << (file.remove ? '1' : '0') << '\t';
		if (!file.remove)
			stream << agi::fs::PathToGenericString(file.staged.filename());
		stream << '\t' << agi::fs::PathToGenericString(file.relative_target) << '\n';
	}
	stream.flush();
	if (!stream)
		throw FileSystemError(
			"Could not write Package transaction journal",
			path,
			std::make_error_code(std::errc::io_error));
	stream.close();
	FlushFileToDisk(path);
	RequireRegularFile(path, "transaction journal");
}

std::vector<PreparedFile> ReadCommitJournal(
	agi::fs::path const& automation_root,
	agi::fs::path const& transaction_root) {
	auto path = transaction_root / agi::fs::PathFromString(std::string(kJournalName));
	RequireRegularFile(path, "transaction journal");
	std::ifstream stream(path, std::ios::binary);
	std::string line;
	if (!std::getline(stream, line) ||
		(line != kJournalHeader && line != kLegacyJournalHeader) ||
		!std::getline(stream, line))
		throw std::runtime_error("Package transaction journal is invalid");
	size_t count = 0;
	auto [end, error] = std::from_chars(line.data(), line.data() + line.size(), count);
	if (error != std::errc{} || end != line.data() + line.size() ||
		count > kMaximumFiles)
		throw std::runtime_error("Package transaction journal count is invalid");
	std::vector<PreparedFile> files;
	files.reserve(count);
	for (size_t index = 0; index < count; ++index) {
		if (!std::getline(stream, line))
			throw std::runtime_error("Package transaction journal is truncated");
		auto first = line.find('\t');
		auto second = first == std::string::npos
			? std::string::npos
			: line.find('\t', first + 1);
		if (first != 1 || second == std::string::npos ||
			line.find('\t', second + 1) != std::string::npos ||
			(line[0] != '0' && line[0] != '1'))
			throw std::runtime_error("Package transaction journal entry is invalid");
		bool remove = line[0] == '1';
		auto staged_value = line.substr(first + 1, second - first - 1);
		auto target_value = line.substr(second + 1);
		auto relative_target = ParseTarget(target_value);
		auto staged = agi::fs::path();
		if (!remove)
			staged = transaction_root / ParseStagedName(staged_value);
		else if (!staged_value.empty())
			throw std::runtime_error(
				"Package transaction removal journal entry has a staged payload");
		files.push_back({
			staged,
			relative_target,
			automation_root / relative_target,
			remove
		});
	}
	if (std::getline(stream, line) && !line.empty())
		throw std::runtime_error("Package transaction journal has trailing data");
	return files;
}

void RecoverTransaction(
	agi::fs::path const& automation_root,
	agi::fs::path const& transaction_root) {
	RequireSafeDirectory(transaction_root);
	auto files = ReadCommitJournal(automation_root, transaction_root);
	auto backup_root = transaction_root / ".native-backup";
	for (size_t offset = 0; offset < files.size(); ++offset) {
		auto index = files.size() - offset - 1;
		auto const& file = files[index];
		auto backup = backup_root /
			agi::fs::PathFromString(std::to_string(index) + ".previous");
		if (Exists(backup)) {
			RequireRegularFile(backup, "transaction backup");
			if (Exists(file.target)) {
				RequireRegularFile(file.target, "recovery target");
				RemoveFileIfPresent(file.target);
			}
			CreateSafeDirectories(automation_root, file.relative_target.parent_path());
			Rename(backup, file.target);
		}
		else if (!file.remove && !Exists(file.staged) && Exists(file.target)) {
			RequireRegularFile(file.target, "recovery target");
			RemoveFileIfPresent(file.target);
		}
	}
	RemoveTreeNoThrow(transaction_root);
}

} // namespace

struct PackageTransactionStore::Impl {
	explicit Impl(
		agi::fs::path root,
		RescanCallback rescan,
		CommitFaultCallback fault)
	: automation_root(std::filesystem::absolute(std::move(root)).lexically_normal())
	, rescan_callback(std::move(rescan))
	, commit_fault_callback(std::move(fault)) {
		if (automation_root.empty())
			throw std::invalid_argument("Package transaction Automation root cannot be empty");
		std::error_code error;
		std::filesystem::create_directories(automation_root, error);
		if (error)
			throw FileSystemError(
				"Could not create Package transaction Automation root", automation_root, error);
		RequireSafeDirectory(automation_root);
		RecoverStagingTransactions();
	}

	void RecoverStagingTransactions() {
		auto staging_base = automation_root / ".dependency-control" / "staging";
		if (!Exists(staging_base))
			return;
		RequireSafeDirectory(staging_base);
		std::error_code error;
		for (std::filesystem::directory_iterator it(staging_base, error), end;
			it != end; it.increment(error)) {
			if (error)
				throw FileSystemError(
					"Could not enumerate Package transaction staging directory",
					staging_base,
					error);
			auto path = it->path();
			if (IsLinkLike(path) || !std::filesystem::is_directory(path, error)) {
				if (error)
					throw FileSystemError(
						"Could not inspect Package transaction staging entry", path, error);
				throw std::runtime_error(
					"Package transaction staging contains an unsafe entry");
			}
			auto journal = path / agi::fs::PathFromString(std::string(kJournalName));
			if (Exists(journal)) {
				RecoverTransaction(automation_root, path);
				++recovered_transactions;
			}
			else {
				RemoveTreeNoThrow(path);
			}
		}
		if (error)
			throw FileSystemError(
				"Could not enumerate Package transaction staging directory",
				staging_base,
				error);
	}

	Transaction& Find(uint64_t plugin_handle, std::string const& id) {
		auto it = transactions.find(id);
		if (it == transactions.end())
			throw std::runtime_error("Unknown Package transaction");
		if (it->second.plugin_handle != plugin_handle)
			throw std::runtime_error(
				"Package transaction belongs to a different plugin");
		return it->second;
	}

	agi::fs::path automation_root;
	RescanCallback rescan_callback;
	CommitFaultCallback commit_fault_callback;
	std::mutex mutex;
	std::map<std::string, Transaction, std::less<>> transactions;
	size_t recovered_transactions = 0;
};

PackageTransactionStore::PackageTransactionStore(
	agi::fs::path automation_root,
	RescanCallback rescan_callback,
	CommitFaultCallback commit_fault_callback)
: impl(std::make_unique<Impl>(
	std::move(automation_root),
	std::move(rescan_callback),
	std::move(commit_fault_callback))) {
}

PackageTransactionStore::~PackageTransactionStore() {
	AbortAll();
}

PackageTransactionStart PackageTransactionStore::Begin(
	uint64_t plugin_handle) {
	if (!plugin_handle)
		throw std::invalid_argument("Package transaction requires a plugin handle");
	std::lock_guard<std::mutex> lock(impl->mutex);

	CreateSafeDirectories(impl->automation_root, ".dependency-control/staging");
	auto staging_base = impl->automation_root / ".dependency-control" / "staging";
	for (int attempt = 0; attempt < 32; ++attempt) {
		auto candidate = agi::fs::UniquePath(staging_base / "tx-%%%%%%%%%%%%%%%%");
		std::error_code error;
		bool created = std::filesystem::create_directory(candidate, error);
		if (error)
			throw FileSystemError(
				"Could not create Package transaction", candidate, error);
		if (!created)
			continue;
		RequireSafeDirectory(candidate);
		auto id = agi::fs::PathToString(candidate.filename());
		impl->transactions.emplace(id, Transaction{plugin_handle, id, candidate});
		return {id, candidate, impl->automation_root};
	}
	throw std::runtime_error("Could not allocate a unique Package transaction");
}

PackageTransactionCommitResult PackageTransactionStore::Commit(
	uint64_t plugin_handle,
	std::string const& transaction_id,
	std::vector<PackageTransactionFile> const& files) {
	if (files.size() > kMaximumFiles)
		throw std::runtime_error("Package transaction exceeds the file limit");

	std::unique_lock<std::mutex> lock(impl->mutex);
	auto& transaction = impl->Find(plugin_handle, transaction_id);
	RequireSafeDirectory(transaction.root);

	std::vector<PreparedFile> prepared;
	prepared.reserve(files.size());
	std::set<std::string, std::less<>> targets;
	for (auto const& file : files) {
		auto relative_target = ParseTarget(file.target);
		auto target_key = agi::fs::PathToGenericString(relative_target);
#ifdef _WIN32
		std::transform(
			target_key.begin(), target_key.end(), target_key.begin(), [](unsigned char value) {
				return value >= 'A' && value <= 'Z'
					? static_cast<char>(value - 'A' + 'a')
					: static_cast<char>(value);
			});
#endif
		if (!targets.insert(std::move(target_key)).second)
			throw std::runtime_error(
				"Package transaction contains duplicate targets");
		auto target = impl->automation_root / relative_target;
		auto staged = agi::fs::path();
		if (!file.remove) {
			staged = transaction.root / ParseStagedName(file.staged_name);
			RequireRegularFile(staged, "staged payload");
		}
		else if (!file.staged_name.empty()) {
			(void)ParseStagedName(file.staged_name);
		}
		prepared.push_back({staged, relative_target, target, file.remove});
	}

	auto backup_root = transaction.root / ".native-backup";
	if (Exists(backup_root))
		throw std::runtime_error(
			"Package transaction contains the reserved backup path");
	CreateSafeDirectory(backup_root);
	WriteCommitJournal(transaction.root, prepared);

	std::vector<AppliedFile> applied;
	applied.reserve(prepared.size());
	std::exception_ptr failure;
	std::string rollback_error;
	try {
		for (size_t index = 0; index < prepared.size(); ++index) {
			auto const& file = prepared[index];
			CreateSafeDirectories(impl->automation_root, file.relative_target.parent_path());
			AppliedFile action;
			action.target = file.target;
			action.backup = backup_root /
				agi::fs::PathFromString(std::to_string(index) + ".previous");

			if (Exists(file.target)) {
				RequireRegularFile(file.target, "existing target");
				Rename(file.target, action.backup);
				action.backed_up = true;
			}
			applied.push_back(action);
			if (!file.remove) {
				Rename(file.staged, file.target);
				applied.back().installed = true;
			}
			if (impl->commit_fault_callback)
				impl->commit_fault_callback(index + 1);
		}
	}
	catch (...) {
		failure = std::current_exception();
		for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
			try {
				if (it->installed)
					RemoveFileIfPresent(it->target);
				if (it->backed_up)
					Rename(it->backup, it->target);
			}
			catch (std::exception const& error) {
				if (rollback_error.empty())
					rollback_error = error.what();
			}
		}
	}

	auto transaction_root = transaction.root;
	impl->transactions.erase(transaction_id);
	RemoveTreeNoThrow(transaction_root);
	if (failure) {
		if (!rollback_error.empty())
			throw std::runtime_error(
				"Package transaction commit failed and rollback also failed: " + rollback_error);
		std::rethrow_exception(failure);
	}

	bool rescan_requested = false;
	auto rescan = impl->rescan_callback;
	lock.unlock();
	if (rescan) {
		try {
			rescan();
			rescan_requested = true;
		}
		catch (...) {
			rescan_requested = false;
		}
	}
	return {files.size(), rescan_requested};
}

void PackageTransactionStore::Abort(
	uint64_t plugin_handle,
	std::string const& transaction_id) {
	std::lock_guard<std::mutex> lock(impl->mutex);
	auto& transaction = impl->Find(plugin_handle, transaction_id);
	auto root = transaction.root;
	impl->transactions.erase(transaction_id);
	RemoveTreeNoThrow(root);
}

void PackageTransactionStore::AbortPlugin(uint64_t plugin_handle) noexcept {
	std::lock_guard<std::mutex> lock(impl->mutex);
	for (auto it = impl->transactions.begin(); it != impl->transactions.end();) {
		if (it->second.plugin_handle != plugin_handle) {
			++it;
			continue;
		}
		auto root = it->second.root;
		it = impl->transactions.erase(it);
		RemoveTreeNoThrow(root);
	}
}

void PackageTransactionStore::AbortAll() noexcept {
	if (!impl)
		return;
	std::lock_guard<std::mutex> lock(impl->mutex);
	for (auto const& [id, transaction] : impl->transactions) {
		(void)id;
		RemoveTreeNoThrow(transaction.root);
	}
	impl->transactions.clear();
}

agi::fs::path const& PackageTransactionStore::AutomationRoot() const noexcept {
	return impl->automation_root;
}

size_t PackageTransactionStore::RecoveredTransactionCount() const noexcept {
	return impl->recovered_transactions;
}

} // namespace Automation4
