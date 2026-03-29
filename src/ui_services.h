#pragma once

#include <libaegisub/background_runner.h>
#include <libaegisub/fs_fwd.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class AudioPlayer;

namespace agi {
class AudioProvider;

class NotificationSink {
public:
	virtual ~NotificationSink() = default;
	virtual void ShowInfo(std::string const& title, std::string const& message) = 0;
	virtual void ShowError(std::string const& title, std::string const& message) = 0;
	virtual void ShowWarning(std::string const& title, std::string const& message) = 0;
};

class NullNotificationSink final : public NotificationSink {
public:
	void ShowInfo(std::string const&, std::string const&) override { }
	void ShowError(std::string const&, std::string const&) override { }
	void ShowWarning(std::string const&, std::string const&) override { }
};

enum class InteractionButtons : int {
	Ok,
	OkCancel,
	YesNo,
	YesNoCancel
};

enum class InteractionIcon : int {
	None,
	Info,
	Warning,
	Error,
	Question
};

enum class InteractionResult : int {
	Ok,
	Cancel,
	Yes,
	No
};

inline InteractionResult DefaultInteractionResult(InteractionButtons buttons) {
	switch (buttons) {
	case InteractionButtons::Ok:
		return InteractionResult::Ok;
	case InteractionButtons::OkCancel:
		return InteractionResult::Cancel;
	case InteractionButtons::YesNo:
		return InteractionResult::No;
	case InteractionButtons::YesNoCancel:
		return InteractionResult::Cancel;
	}
	return InteractionResult::Cancel;
}

struct InteractionRequest {
	std::string title;
	std::string message;
	InteractionButtons buttons = InteractionButtons::Ok;
	InteractionIcon icon = InteractionIcon::None;
};

class InteractionSink {
public:
	virtual ~InteractionSink() = default;
	virtual InteractionResult Request(InteractionRequest const& request) = 0;
};

class NullInteractionSink final : public InteractionSink {
public:
	InteractionResult Request(InteractionRequest const& request) override {
		return DefaultInteractionResult(request.buttons);
	}
};

struct SingleChoiceInteractionRequest {
	std::string title;
	std::string message;
	std::vector<std::string> choices;
	int default_choice = 0;
	std::string help_page;
	std::string request_id;
};

class SingleChoiceInteractionSink {
public:
	virtual ~SingleChoiceInteractionSink() = default;
	virtual std::optional<int> RequestSingleChoice(SingleChoiceInteractionRequest const& request) = 0;
};

class NullSingleChoiceInteractionSink final : public SingleChoiceInteractionSink {
public:
	std::optional<int> RequestSingleChoice(SingleChoiceInteractionRequest const&) override {
		return std::nullopt;
	}
};

struct OpenFileDialogRequest {
	std::string title;
	std::string option_name;
	std::string default_filename;
	std::string default_extension;
	std::string wildcard;
	std::string default_path;
	bool must_exist = true;
};

struct OpenFilesDialogRequest {
	std::string title;
	std::string option_name;
	std::string default_filename;
	std::string default_extension;
	std::string wildcard;
	std::string default_path;
	bool must_exist = true;
};

struct SaveFileDialogRequest {
	std::string title;
	std::string option_name;
	std::string default_filename;
	std::string default_extension;
	std::string wildcard;
	std::string default_path;
	bool prompt_overwrite = true;
};

struct SelectDirectoryDialogRequest {
	std::string title;
	std::string default_path;
};

class FileDialogService {
public:
	virtual ~FileDialogService() = default;
	virtual agi::fs::path RequestOpenFile(OpenFileDialogRequest const& request) = 0;
	virtual std::vector<agi::fs::path> RequestOpenFiles(OpenFilesDialogRequest const& request) = 0;
	virtual agi::fs::path RequestSaveFile(SaveFileDialogRequest const& request) = 0;
	virtual agi::fs::path RequestSelectDirectory(SelectDirectoryDialogRequest const& request) = 0;
};

class NullFileDialogService final : public FileDialogService {
public:
	agi::fs::path RequestOpenFile(OpenFileDialogRequest const&) override {
		return {};
	}

	std::vector<agi::fs::path> RequestOpenFiles(OpenFilesDialogRequest const&) override {
		return {};
	}

	agi::fs::path RequestSaveFile(SaveFileDialogRequest const&) override {
		return {};
	}

	agi::fs::path RequestSelectDirectory(SelectDirectoryDialogRequest const&) override {
		return {};
	}
};

class VideoSourceRequestService {
public:
	virtual ~VideoSourceRequestService() = default;
	virtual std::string RequestDummyVideoPath() = 0;
};

class NullVideoSourceRequestService final : public VideoSourceRequestService {
public:
	std::string RequestDummyVideoPath() override {
		return {};
	}
};

class BackgroundRunnerFactory {
public:
	virtual ~BackgroundRunnerFactory() = default;
	virtual std::unique_ptr<BackgroundRunner> Create(std::string const& title, std::string const& message) = 0;
};

struct ProjectUiStateSnapshot {
	std::optional<int> subtitle_scroll_position;
	std::optional<double> video_zoom;
};

class ProjectUiStateSink {
public:
	virtual ~ProjectUiStateSink() = default;
	virtual void RestoreProjectUiState(ProjectUiStateSnapshot const& state) = 0;
};

class NullProjectUiStateSink final : public ProjectUiStateSink {
public:
	void RestoreProjectUiState(ProjectUiStateSnapshot const&) override { }
};

class AudioPlayerFactoryService {
public:
	virtual ~AudioPlayerFactoryService() = default;
	virtual std::unique_ptr<::AudioPlayer> CreateAudioPlayer(AudioProvider *provider) = 0;
};

class NullAudioPlayerFactoryService final : public AudioPlayerFactoryService {
public:
	std::unique_ptr<::AudioPlayer> CreateAudioPlayer(AudioProvider *) override;
};

namespace detail {
class InlineProgressSink final : public ProgressSink {
public:
	void SetIndeterminate() override { }
	void SetTitle(std::string const&) override { }
	void SetMessage(std::string const&) override { }
	void SetProgress(int64_t, int64_t) override { }
	void Log(std::string const&) override { }
	bool IsCancelled() override { return false; }
};

class InlineBackgroundRunner final : public BackgroundRunner {
public:
	void Run(std::function<void(ProgressSink *)> task) override {
		InlineProgressSink sink;
		task(&sink);
	}
};
}

class InlineBackgroundRunnerFactory final : public BackgroundRunnerFactory {
public:
	std::unique_ptr<BackgroundRunner> Create(std::string const&, std::string const&) override {
		return std::make_unique<detail::InlineBackgroundRunner>();
	}
};

}
