// Dual-process smoke: two concurrent LoadOrRebuildWindowsFontFamilyCatalog
// cold starts against the same cache path must perform only one full Observe
// (the other must admit a disk hit after the rebuild lock). Bounded overall
// timeout; not a CI-required matrix test by default (machine font set).

#include "../../src/font_family_obs_repository_win.h"
#include "../../src/gdi_font_resolver.h"
#include "../../src/options.h"

#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kWorkerTimeoutSeconds = 180;
constexpr int kOverallTimeoutSeconds = 240;

struct WorkerResult {
	std::string hit;
	std::uint64_t physical_probe_count = 0;
	std::size_t catalog_size = 0;
	bool wrote_cache = false;
	bool ok = false;
};

int Fail(std::string const& message) {
	std::cerr << "font-family-obs-lock-smoke: " << message << '\n';
	return 1;
}

void WriteResult(fs::path const& path, FontFamilyObsRepositoryResult const& result) {
	std::ofstream out(path, std::ios::binary);
	out << "hit=" << FontFamilyObsHitKindName(result.hit) << '\n'
	    << "physical_probe_count=" << result.physical_probe_count << '\n'
	    << "catalog_size=" << result.catalog.size() << '\n'
	    << "wrote_cache=" << (result.wrote_cache ? 1 : 0) << '\n'
	    << "ok=1\n";
}

WorkerResult ReadResult(fs::path const& path) {
	WorkerResult result;
	std::ifstream in(path);
	if (!in)
		return result;
	std::string line;
	while (std::getline(in, line)) {
		auto const eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		auto const key = line.substr(0, eq);
		auto const value = line.substr(eq + 1);
		if (key == "hit")
			result.hit = value;
		else if (key == "physical_probe_count")
			result.physical_probe_count = std::stoull(value);
		else if (key == "catalog_size")
			result.catalog_size = static_cast<std::size_t>(std::stoull(value));
		else if (key == "wrote_cache")
			result.wrote_cache = value == "1";
		else if (key == "ok")
			result.ok = value == "1";
	}
	return result;
}

class ScopedLocalPath {
	std::unique_ptr<agi::Path> path_;
	agi::Path* previous_ = nullptr;

public:
	explicit ScopedLocalPath(agi::fs::path const& local_root) {
		previous_ = config::path;
		path_ = std::make_unique<agi::Path>();
		path_->SetToken("?local", local_root);
		path_->SetToken("?local", local_root);
		config::path = path_.get();
	}
	~ScopedLocalPath() { config::path = previous_; }
};

int RunWorker(fs::path const& work_dir, fs::path const& out_path) {
	agi::log::log = new agi::log::LogSink;
	ScopedLocalPath scoped(work_dir);
	auto const cache_path = DefaultWindowsFontFamilyObsCachePath();
	if (cache_path.empty())
		return Fail("worker: empty cache path");

	// Production wait is 3s; a full Observe often exceeds that. This smoke
	// exercises "exactly one Observe under the lock", not the timeout fallback,
	// so extend the wait for the peer rebuild only in this process.
	SetFontFamilyObsRebuildLockWaitMsForTest(120000);

	GdiFontResolver resolver;
	auto const result = LoadOrRebuildWindowsFontFamilyCatalog(
		resolver, "en-US", [] { return false; }, cache_path);
	WriteResult(out_path, result);
	if (result.hit == FontFamilyObsHitKind::MissLockTimeout)
		return Fail("worker: MissLockTimeout (peer did not finish in lock window)");
	if (result.catalog.empty())
		return Fail("worker: empty catalog");
	return 0;
}

std::wstring ToWide(std::string const& utf8) {
	if (utf8.empty())
		return {};
	int const n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
		static_cast<int>(utf8.size()), nullptr, 0);
	if (n <= 0)
		return {};
	std::wstring wide(static_cast<std::size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
		wide.data(), n);
	return wide;
}

bool SpawnWorker(
	fs::path const& self,
	fs::path const& work_dir,
	fs::path const& out_path,
	PROCESS_INFORMATION& pi) {
	std::string cmd = "\"" + self.string() + "\" --worker --work-dir \"" +
		work_dir.string() + "\" --out \"" + out_path.string() + "\"";
	auto wide_cmd = ToWide(cmd);
	if (wide_cmd.empty())
		return false;
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));
	// CreateProcessW may modify the command line buffer.
	return CreateProcessW(
		nullptr, wide_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
		&si, &pi) != FALSE;
}

int RunCoordinator(fs::path const& self, fs::path work_dir) {
	std::error_code ec;
	fs::create_directories(work_dir, ec);
	if (ec)
		return Fail("cannot create work dir");

	// Isolate ?local so DefaultWindowsFontFamilyObsCachePath is under work_dir.
	ScopedLocalPath scoped(work_dir);
	auto const cache_path = DefaultWindowsFontFamilyObsCachePath();
	if (cache_path.empty())
		return Fail("coordinator: empty cache path");
	try {
		agi::fs::Remove(cache_path);
	}
	catch (...) {
	}
	// Ensure parent of cache exists.
	try {
		agi::fs::CreateDirectory(cache_path.parent_path());
	}
	catch (...) {
	}

	auto const out1 = work_dir / "worker1.txt";
	auto const out2 = work_dir / "worker2.txt";
	fs::remove(out1, ec);
	fs::remove(out2, ec);

	PROCESS_INFORMATION pi1{};
	PROCESS_INFORMATION pi2{};
	if (!SpawnWorker(self, work_dir, out1, pi1))
		return Fail("failed to spawn worker 1");
	if (!SpawnWorker(self, work_dir, out2, pi2)) {
		TerminateProcess(pi1.hProcess, 1);
		CloseHandle(pi1.hProcess);
		CloseHandle(pi1.hThread);
		return Fail("failed to spawn worker 2");
	}

	HANDLE waits[2] = {pi1.hProcess, pi2.hProcess};
	auto const wait = WaitForMultipleObjects(
		2, waits, TRUE, static_cast<DWORD>(kOverallTimeoutSeconds * 1000));
	if (wait != WAIT_OBJECT_0 && wait != WAIT_OBJECT_0 + 1) {
		TerminateProcess(pi1.hProcess, 1);
		TerminateProcess(pi2.hProcess, 1);
		CloseHandle(pi1.hProcess);
		CloseHandle(pi1.hThread);
		CloseHandle(pi2.hProcess);
		CloseHandle(pi2.hThread);
		return Fail("workers timed out");
	}

	DWORD code1 = 1;
	DWORD code2 = 1;
	GetExitCodeProcess(pi1.hProcess, &code1);
	GetExitCodeProcess(pi2.hProcess, &code2);
	CloseHandle(pi1.hProcess);
	CloseHandle(pi1.hThread);
	CloseHandle(pi2.hProcess);
	CloseHandle(pi2.hThread);

	if (code1 != 0 || code2 != 0)
		return Fail("worker exit non-zero (code1=" + std::to_string(code1) +
			" code2=" + std::to_string(code2) + ")");

	auto const r1 = ReadResult(out1);
	auto const r2 = ReadResult(out2);
	if (!r1.ok || !r2.ok)
		return Fail("missing worker result files");

	std::cout << "font-family-obs-lock-smoke: worker1 hit=" << r1.hit
	          << " probes=" << r1.physical_probe_count
	          << " catalog=" << r1.catalog_size
	          << " wrote_cache=" << r1.wrote_cache << '\n';
	std::cout << "font-family-obs-lock-smoke: worker2 hit=" << r2.hit
	          << " probes=" << r2.physical_probe_count
	          << " catalog=" << r2.catalog_size
	          << " wrote_cache=" << r2.wrote_cache << '\n';

	if (r1.catalog_size == 0 || r2.catalog_size == 0)
		return Fail("expected non-empty catalogs from both workers");

	// Exactly one process should have performed the physical Observe rebuild.
	int rebuilders = 0;
	if (r1.physical_probe_count > 0)
		++rebuilders;
	if (r2.physical_probe_count > 0)
		++rebuilders;
	if (rebuilders != 1) {
		return Fail(
			"expected exactly one worker with physical_probe_count > 0, got " +
			std::to_string(rebuilders));
	}

	// The non-builder should have admitted a hit with zero probes.
	auto const& waiter = r1.physical_probe_count == 0 ? r1 : r2;
	if (waiter.hit != "hit") {
		return Fail(
			"expected non-building worker hit=hit, got hit=" + waiter.hit);
	}
	if (waiter.physical_probe_count != 0)
		return Fail("waiter must have zero physical probes");

	std::cout << "font-family-obs-lock-smoke: PASS (single full Observe under lock)\n";
	return 0;
}

void PrintUsage(char const* argv0) {
	std::cerr
		<< "Usage:\n"
		<< "  " << argv0 << " [--work-dir DIR]\n"
		<< "  " << argv0 << " --worker --work-dir DIR --out FILE\n";
}

} // namespace

int main(int argc, char** argv) {
	fs::path work_dir;
	fs::path out_path;
	bool worker = false;
	for (int i = 1; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "--help" || arg == "-h") {
			PrintUsage(argv[0]);
			return 0;
		}
		if (arg == "--worker") {
			worker = true;
			continue;
		}
		if (arg == "--work-dir") {
			if (i + 1 >= argc)
				return Fail("--work-dir requires a path");
			work_dir = argv[++i];
			continue;
		}
		if (arg == "--out") {
			if (i + 1 >= argc)
				return Fail("--out requires a path");
			out_path = argv[++i];
			continue;
		}
		return Fail(std::string("unknown argument: ") + std::string(arg));
	}

	wchar_t module[MAX_PATH] = {};
	if (GetModuleFileNameW(nullptr, module, MAX_PATH) == 0)
		return Fail("GetModuleFileNameW failed");
	fs::path const self = module;

	if (worker) {
		if (work_dir.empty() || out_path.empty())
			return Fail("worker requires --work-dir and --out");
		return RunWorker(work_dir, out_path);
	}

	if (work_dir.empty()) {
		work_dir = fs::path("build-dir") / "font-family-obs-lock-smoke" /
			std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count());
	}
	return RunCoordinator(self, work_dir);
}
