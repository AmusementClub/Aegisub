// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <memory>
#include <string>

class UiTimer;

enum class FileSystemWatchEventKind {
	Created,
	Deleted,
	Modified,
	Renamed
};

struct FileSystemWatchEvent {
	FileSystemWatchEventKind kind = FileSystemWatchEventKind::Modified;
	agi::fs::path path;
	agi::fs::path new_path;
};

class FileSystemWatcherListener {
public:
	virtual ~FileSystemWatcherListener() = default;
	virtual void OnFileSystemWatchEvent(FileSystemWatchEvent const& event) = 0;
	virtual void OnFileSystemWatchError(std::string const& message) = 0;
};

class FileSystemWatcherBackend {
public:
	virtual ~FileSystemWatcherBackend() = default;
	virtual bool WatchDirectory(agi::fs::path const& directory, FileSystemWatcherListener* listener) = 0;
	virtual void Reset() = 0;
};

std::unique_ptr<FileSystemWatcherBackend> CreateDefaultFileSystemWatcherBackend();

class WatchedFile final : private FileSystemWatcherListener {
	std::unique_ptr<FileSystemWatcherBackend> backend;
	std::unique_ptr<UiTimer> debounce_timer;
	agi::fs::path target_path;
	agi::fs::path watched_directory;
	std::function<void(agi::fs::path const&)> changed_callback;
	std::function<void(std::string const&)> error_callback;

	void NotifyError(std::string const& message);
	bool EventAffectsTarget(FileSystemWatchEvent const& event) const;

	void OnFileSystemWatchEvent(FileSystemWatchEvent const& event) override;
	void OnFileSystemWatchError(std::string const& message) override;

public:
	explicit WatchedFile(std::unique_ptr<FileSystemWatcherBackend> backend = CreateDefaultFileSystemWatcherBackend());
	~WatchedFile();

	bool SetTargetPath(agi::fs::path const& path);
	void ClearTargetPath();

	agi::fs::path const& GetTargetPath() const { return target_path; }
	agi::fs::path const& GetWatchedDirectory() const { return watched_directory; }

	void SetChangedCallback(std::function<void(agi::fs::path const&)> callback) {
		changed_callback = std::move(callback);
	}

	void SetErrorCallback(std::function<void(std::string const&)> callback) {
		error_callback = std::move(callback);
	}
};
