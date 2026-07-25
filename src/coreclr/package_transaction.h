#pragma once

#include <libaegisub/fs_fwd.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Automation4 {

struct PackageTransactionStart {
	std::string transaction_id;
	agi::fs::path staging_root;
	agi::fs::path automation_root;
};

struct PackageTransactionFile {
	std::string staged_name;
	std::string target;
	bool remove = false;
};

struct PackageTransactionCommitResult {
	size_t file_count = 0;
	bool rescan_requested = false;
};

class PackageTransactionStore final {
public:
	using RescanCallback = std::function<void()>;
	using CommitFaultCallback = std::function<void(size_t)>;

	PackageTransactionStore(
		agi::fs::path automation_root,
		RescanCallback rescan_callback = {},
		CommitFaultCallback commit_fault_callback = {});
	~PackageTransactionStore();

	PackageTransactionStore(PackageTransactionStore const&) = delete;
	PackageTransactionStore& operator=(PackageTransactionStore const&) = delete;

	PackageTransactionStart Begin(uint64_t plugin_handle);
	PackageTransactionCommitResult Commit(
		uint64_t plugin_handle,
		std::string const& transaction_id,
		std::vector<PackageTransactionFile> const& files);
	void Abort(uint64_t plugin_handle, std::string const& transaction_id);
	void AbortPlugin(uint64_t plugin_handle) noexcept;
	void AbortAll() noexcept;

	agi::fs::path const& AutomationRoot() const noexcept;
	size_t RecoveredTransactionCount() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace Automation4
