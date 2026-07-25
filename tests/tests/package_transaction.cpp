#include <main.h>

#include "../../src/coreclr/package_transaction.h"

#include <libaegisub/fs.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

class TransactionTestRoot {
	agi::fs::path root;

public:
	explicit TransactionTestRoot(std::string const& prefix = "dependency-control")
	: root(std::filesystem::absolute(
		agi::fs::UniquePath(
			agi::fs::PathFromString("data/" + prefix + "-%%%%%%%%%%%%%%%%")))) {
		std::error_code error;
		std::filesystem::remove_all(root, error);
		std::filesystem::create_directories(root);
	}

	~TransactionTestRoot() {
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}

	agi::fs::path const& Path() const { return root; }
};

void WriteFile(agi::fs::path const& path, std::string const& value) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	if (!stream)
		throw std::runtime_error(
			"Could not create transaction test file " + agi::fs::PathToString(path));
	stream << value;
	if (!stream)
		throw std::runtime_error(
			"Could not write transaction test file " + agi::fs::PathToString(path));
}

std::string ReadFile(agi::fs::path const& path) {
	std::ifstream stream(path, std::ios::binary);
	if (!stream)
		throw std::runtime_error(
			"Could not read transaction test file " + agi::fs::PathToString(path));
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

} // namespace

TEST(package_transaction, commits_replace_delete_and_rescan_once) {
	TransactionTestRoot test_root;
	int rescans = 0;
	Automation4::PackageTransactionStore store(
		test_root.Path(), [&] { ++rescans; });
	auto existing = test_root.Path() / "include" / "sample" / "module.lua";
	auto deleted = test_root.Path() / "include" / "sample" / "obsolete.lua";
	WriteFile(existing, "old");
	WriteFile(deleted, "obsolete");

	auto transaction = store.Begin(7);
	WriteFile(transaction.staging_root / "0001.payload", "new");
	auto result = store.Commit(7, transaction.transaction_id, {
		{"0001.payload", "include/sample/module.lua", false},
		{"", "include/sample/obsolete.lua", true}
	});

	EXPECT_EQ("new", ReadFile(existing));
	EXPECT_FALSE(std::filesystem::exists(deleted));
	EXPECT_FALSE(std::filesystem::exists(transaction.staging_root));
	EXPECT_EQ(2u, result.file_count);
	EXPECT_TRUE(result.rescan_requested);
	EXPECT_EQ(1, rescans);
}

TEST(package_transaction, commit_failure_restores_all_previous_files) {
	TransactionTestRoot test_root;
	int rescans = 0;
	Automation4::PackageTransactionStore store(
		test_root.Path(),
		[&] { ++rescans; },
		[](size_t completed) {
			if (completed == 1)
				throw std::runtime_error("injected transaction failure");
		});
	auto first = test_root.Path() / "include" / "sample" / "first.lua";
	auto second = test_root.Path() / "include" / "sample" / "second.lua";
	WriteFile(first, "old-first");

	auto transaction = store.Begin(9);
	WriteFile(transaction.staging_root / "0001.payload", "new-first");
	WriteFile(transaction.staging_root / "0002.payload", "new-second");
	EXPECT_THROW(
		store.Commit(9, transaction.transaction_id, {
			{"0001.payload", "include/sample/first.lua", false},
			{"0002.payload", "include/sample/second.lua", false}
		}),
		std::runtime_error);

	EXPECT_EQ("old-first", ReadFile(first));
	EXPECT_FALSE(std::filesystem::exists(second));
	EXPECT_FALSE(std::filesystem::exists(transaction.staging_root));
	EXPECT_EQ(0, rescans);
}

TEST(package_transaction, rejects_unsafe_and_duplicate_targets) {
	TransactionTestRoot test_root;
	Automation4::PackageTransactionStore store(test_root.Path());
	auto transaction = store.Begin(11);
	WriteFile(transaction.staging_root / "0001.payload", "one");
	WriteFile(transaction.staging_root / "0002.payload", "two");

	EXPECT_THROW(
		store.Commit(11, transaction.transaction_id, {
			{"0001.payload", "../outside.lua", false}
		}),
		std::runtime_error);
	EXPECT_THROW(
		store.Commit(11, transaction.transaction_id, {
			{"0001.payload", "include/sample.lua", false},
			{"0002.payload", "include/sample.lua", false}
		}),
		std::runtime_error);
#ifdef _WIN32
	EXPECT_THROW(
		store.Commit(11, transaction.transaction_id, {
			{"0001.payload", "include/Sample.lua", false},
			{"0002.payload", "include/sample.lua", false}
		}),
		std::runtime_error);
	EXPECT_THROW(
		store.Commit(11, transaction.transaction_id, {
			{"0001.payload", "include/CON.lua", false}
		}),
		std::runtime_error);
#endif
	store.Abort(11, transaction.transaction_id);
	EXPECT_FALSE(std::filesystem::exists(transaction.staging_root));
}

TEST(package_transaction, transaction_is_bound_to_plugin_handle) {
	TransactionTestRoot test_root;
	Automation4::PackageTransactionStore store(test_root.Path());
	auto transaction = store.Begin(13);

	EXPECT_THROW(store.Abort(14, transaction.transaction_id), std::runtime_error);
	EXPECT_TRUE(std::filesystem::exists(transaction.staging_root));
	store.AbortPlugin(13);
	EXPECT_FALSE(std::filesystem::exists(transaction.staging_root));
}

TEST(package_transaction, supports_unicode_automation_root) {
	TransactionTestRoot test_root("dependency-control-\xF0\x9F\x98\x80");
	Automation4::PackageTransactionStore store(test_root.Path());
	auto transaction = store.Begin(17);
	WriteFile(transaction.staging_root / "payload.lua", "return true");

	EXPECT_NO_THROW(store.Commit(17, transaction.transaction_id, {
		{"payload.lua", "include/unicode/module.lua", false}
	}));
	EXPECT_EQ(
		"return true",
		ReadFile(test_root.Path() / "include" / "unicode" / "module.lua"));
}

TEST(package_transaction, rejects_linked_target_directory) {
	TransactionTestRoot test_root;
	auto outside = test_root.Path() / "outside";
	auto include = test_root.Path() / "include";
	std::filesystem::create_directories(outside);
	std::error_code error;
	std::filesystem::create_directory_symlink(outside, include, error);
	if (error)
		GTEST_SKIP() << "Directory symlinks are unavailable in this test environment";

	Automation4::PackageTransactionStore store(test_root.Path());
	auto transaction = store.Begin(19);
	WriteFile(transaction.staging_root / "payload.lua", "return true");
	EXPECT_THROW(store.Commit(19, transaction.transaction_id, {
		{"payload.lua", "include/escaped.lua", false}
	}), std::runtime_error);
	EXPECT_FALSE(std::filesystem::exists(transaction.staging_root));
	EXPECT_FALSE(std::filesystem::exists(outside / "escaped.lua"));
}

TEST(package_transaction, recovers_journaled_interrupted_commit) {
	TransactionTestRoot test_root;
	auto staging = test_root.Path() / ".dependency-control" / "staging" / "tx-orphan";
	auto backup = staging / ".native-backup";
	auto first = test_root.Path() / "include" / "sample" / "first.lua";
	auto second = test_root.Path() / "include" / "sample" / "second.lua";
	auto deleted = test_root.Path() / "include" / "sample" / "deleted.lua";
	auto added = test_root.Path() / "include" / "sample" / "added.lua";
	WriteFile(first, "new-first");
	WriteFile(second, "old-second");
	WriteFile(added, "new-added");
	WriteFile(backup / "0.previous", "old-first");
	WriteFile(backup / "2.previous", "old-deleted");
	WriteFile(staging / "payload-second", "new-second");
	WriteFile(
		staging / ".native-journal-v1",
		"Aegisub.Package.Transaction/1\n"
		"4\n"
		"0\tpayload-first\tinclude/sample/first.lua\n"
		"0\tpayload-second\tinclude/sample/second.lua\n"
		"1\t\tinclude/sample/deleted.lua\n"
		"0\tpayload-added\tinclude/sample/added.lua\n");

	Automation4::PackageTransactionStore store(test_root.Path());
	EXPECT_EQ(1u, store.RecoveredTransactionCount());
	EXPECT_EQ("old-first", ReadFile(first));
	EXPECT_EQ("old-second", ReadFile(second));
	EXPECT_EQ("old-deleted", ReadFile(deleted));
	EXPECT_FALSE(std::filesystem::exists(added));
	EXPECT_FALSE(std::filesystem::exists(staging));
}

TEST(package_transaction, removes_unjournaled_abandoned_staging) {
	TransactionTestRoot test_root;
	auto staging = test_root.Path() / ".dependency-control" / "staging" / "tx-abandoned";
	WriteFile(staging / "payload.lua", "not committed");

	Automation4::PackageTransactionStore store(test_root.Path());
	EXPECT_EQ(0u, store.RecoveredTransactionCount());
	EXPECT_FALSE(std::filesystem::exists(staging));
}

TEST(package_transaction, preserves_invalid_journal_for_manual_recovery) {
	TransactionTestRoot test_root;
	auto staging = test_root.Path() / ".dependency-control" / "staging" / "tx-invalid";
	WriteFile(staging / ".native-journal-v1", "invalid\n");

	EXPECT_THROW(
		Automation4::PackageTransactionStore(test_root.Path()),
		std::runtime_error);
	EXPECT_TRUE(std::filesystem::exists(staging));
}
