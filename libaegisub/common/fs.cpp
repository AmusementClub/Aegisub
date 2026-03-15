// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "libaegisub/fs.h"

#include "libaegisub/access.h"
#include "libaegisub/log.h"
#include "libaegisub/string_utils.h"

#include <chrono>
#include <filesystem>
#include <random>

namespace bfs = std::filesystem;

// filesystem functions throw a single exception type for all
// errors, which isn't really what we want, so do some crazy wrapper
// shit to map error codes to more useful exceptions.
#ifdef _WIN32
#include <winerror.h>
#define CHECKED_CALL(exp, src_path, dst_path) \
	std::error_code ec; \
	exp; \
	switch (ec.value()) {\
		case ERROR_SUCCESS: break; \
		case ERROR_FILE_NOT_FOUND: throw FileNotFound(src_path); \
		case ERROR_DIRECTORY: throw NotADirectory(src_path); \
		case ERROR_DISK_FULL: throw DriveFull(dst_path); \
		case ERROR_ACCESS_DENIED: \
			if (!src_path.empty()) \
				acs::CheckFileRead(src_path); \
			if (!dst_path.empty()) \
				acs::CheckFileWrite(dst_path); \
			throw AccessDenied(src_path); \
		default: \
			LOG_D("filesystem") << "Unknown error when calling '" << #exp << "': " << ec << ": " << ec.message(); \
			throw FileSystemUnknownError(ec.message()); \
	}
#else
#define CHECKED_CALL(exp, src_path, dst_path) \
	std::error_code ec; \
	exp; \
	if (!ec) { \
	} \
	else if (ec == std::errc::no_such_file_or_directory) throw FileNotFound(src_path); \
	else if (ec == std::errc::is_a_directory) throw NotAFile(src_path); \
	else if (ec == std::errc::not_a_directory) throw NotADirectory(src_path); \
	else if (ec == std::errc::no_space_on_device) throw DriveFull(dst_path); \
	else if (ec == std::errc::permission_denied) { \
			if (!src_path.empty()) \
				acs::CheckFileRead(src_path); \
			if (!dst_path.empty()) \
				acs::CheckFileWrite(dst_path); \
			throw AccessDenied(src_path); \
	} \
	else { \
		LOG_D("filesystem") << "Unknown error when calling '" << #exp << "': " << ec << ": " << ec.message(); \
		throw FileSystemUnknownError(ec.message()); \
	}
#endif

#define CHECKED_CALL_RETURN(exp, src_path) \
	CHECKED_CALL(auto ret = exp, src_path, agi::fs::path()); \
	return ret

#define WRAP_BFS(bfs_name, agi_name) \
	auto agi_name(path const& p) -> decltype(bfs::bfs_name(p)) { \
		CHECKED_CALL_RETURN(bfs::bfs_name(p, ec), p); \
	}

#define WRAP_BFS_IGNORE_ERROR(bfs_name, agi_name) \
	auto agi_name(path const& p) -> decltype(bfs::bfs_name(p)) { \
		std::error_code ec; \
		return bfs::bfs_name(p, ec); \
	}

// sasuga windows.h
#undef CreateDirectory

namespace agi { namespace fs {
namespace {
	void ThrowFileSystemError(std::error_code const& ec, path const& src_path, path const& dst_path) {
#ifdef _WIN32
		switch (ec.value()) {
			case ERROR_SUCCESS: return;
			case ERROR_FILE_NOT_FOUND: throw FileNotFound(src_path);
			case ERROR_DIRECTORY: throw NotADirectory(src_path);
			case ERROR_DISK_FULL: throw DriveFull(dst_path);
			case ERROR_ACCESS_DENIED:
				if (!src_path.empty())
					acs::CheckFileRead(src_path);
				if (!dst_path.empty())
					acs::CheckFileWrite(dst_path);
				throw AccessDenied(src_path);
			default:
				LOG_D("filesystem") << "Unknown error: " << ec << ": " << ec.message();
				throw FileSystemUnknownError(ec.message());
		}
#else
		if (!ec)
			return;
		if (ec == std::errc::no_such_file_or_directory) throw FileNotFound(src_path);
		if (ec == std::errc::is_a_directory) throw NotAFile(src_path);
		if (ec == std::errc::not_a_directory) throw NotADirectory(src_path);
		if (ec == std::errc::no_space_on_device) throw DriveFull(dst_path);
		if (ec == std::errc::permission_denied) {
			if (!src_path.empty())
				acs::CheckFileRead(src_path);
			if (!dst_path.empty())
				acs::CheckFileWrite(dst_path);
			throw AccessDenied(src_path);
		}
		LOG_D("filesystem") << "Unknown error: " << ec << ": " << ec.message();
		throw FileSystemUnknownError(ec.message());
#endif
	}

	WRAP_BFS(file_size, SizeImpl)
	WRAP_BFS(space, Space)
}

	WRAP_BFS_IGNORE_ERROR(exists, Exists)
	WRAP_BFS_IGNORE_ERROR(is_regular_file, FileExists)
	WRAP_BFS_IGNORE_ERROR(is_directory, DirectoryExists)
	WRAP_BFS(create_directories, CreateDirectory)
	WRAP_BFS(remove, Remove)
	WRAP_BFS(canonical, Canonicalize)

	uintmax_t Size(path const& p) {
		if (DirectoryExists(p))
			throw NotAFile(p);
		return SizeImpl(p);
	}

	uintmax_t FreeSpace(path const& p) {
		return Space(p).available;
	}

	time_t ModifiedTime(path const& p) {
		std::error_code ec;
		auto ret = bfs::last_write_time(p, ec);
		if (ec)
			ThrowFileSystemError(ec, p, agi::fs::path());
		auto sys_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
			ret - bfs::file_time_type::clock::now() + std::chrono::system_clock::now());
		return std::chrono::system_clock::to_time_t(sys_time);
	}

	void Rename(const path& from, const path& to) {
		CHECKED_CALL(bfs::rename(from, to, ec), from, to);
	}

	path UniquePath(path const& model) {
		static thread_local std::mt19937_64 rng(std::random_device{}());
		static constexpr char hex[] = "0123456789abcdef";
		auto result = model.native();
		for (auto& ch : result) {
			if (ch == path::value_type('%'))
				ch = static_cast<path::value_type>(hex[rng() & 0xF]);
		}
		return path(result);
	}

	bool HasExtension(path const& p, std::string const& ext) {
		auto filename = p.filename().string();
		if (filename.size() < ext.size() + 1) return false;
		if (filename[filename.size() - ext.size() - 1] != '.') return false;
		return agi::util::strings::iends_with(filename, ext);
	}
} }
