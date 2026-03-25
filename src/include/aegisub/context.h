// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
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

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <libaegisub/fs_fwd.h>

class AssFile;
class AudioBox;
class AudioController;
class AssDialogue;
class AudioKaraoke;
class DialogManager;
class FrameMain;
class Project;
class SearchReplaceEngine;
class InitialLineState;
class SelectionController;
class SubsController;
class BaseGrid;
class TextSelectionController;
class VideoController;
class VideoDisplay;
class wxWindow;
namespace Automation4 { class ScriptManager; }
namespace agi { class BackgroundRunner; }
namespace agi { class StatusSink; }
namespace agi { class NotificationSink; }
namespace agi { class InteractionSink; }
namespace agi { class SingleChoiceInteractionSink; }
namespace agi { class FileDialogService; }
namespace agi { class VideoSourceRequestService; }
namespace agi { class BackgroundRunnerFactory; }
namespace agi { struct InteractionRequest; }
namespace agi { struct SingleChoiceInteractionRequest; }
namespace agi { struct OpenFileDialogRequest; }
namespace agi { struct OpenFilesDialogRequest; }
namespace agi { struct SaveFileDialogRequest; }
namespace agi { struct SelectDirectoryDialogRequest; }
namespace agi { enum class InteractionResult : int; }

namespace agi {
class Path;
struct Context;

struct ContextCoreSession {
	std::unique_ptr<AssFile>& ass;
	std::unique_ptr<TextSelectionController>& textSelectionController;
	std::unique_ptr<SubsController>& subsController;
	std::unique_ptr<Project>& project;
	std::unique_ptr<Automation4::ScriptManager>& local_scripts;
	std::unique_ptr<SelectionController>& selectionController;
	std::unique_ptr<VideoController>& videoController;
	std::unique_ptr<AudioController>& audioController;
	std::unique_ptr<InitialLineState>& initialLineState;
	std::unique_ptr<SearchReplaceEngine>& search;
	std::unique_ptr<Path>& path;
	std::shared_ptr<StatusSink>& statusSink;
	std::shared_ptr<NotificationSink>& notificationSink;
	std::shared_ptr<InteractionSink>& interactionSink;
	std::shared_ptr<SingleChoiceInteractionSink>& singleChoiceInteractionSink;
	std::shared_ptr<FileDialogService>& fileDialogService;
	std::shared_ptr<VideoSourceRequestService>& videoSourceRequestService;
	std::shared_ptr<BackgroundRunnerFactory>& backgroundRunnerFactory;

	explicit ContextCoreSession(Context& context);
};

struct ConstContextCoreSession {
	std::unique_ptr<AssFile> const& ass;
	std::unique_ptr<TextSelectionController> const& textSelectionController;
	std::unique_ptr<SubsController> const& subsController;
	std::unique_ptr<Project> const& project;
	std::unique_ptr<Automation4::ScriptManager> const& local_scripts;
	std::unique_ptr<SelectionController> const& selectionController;
	std::unique_ptr<VideoController> const& videoController;
	std::unique_ptr<AudioController> const& audioController;
	std::unique_ptr<InitialLineState> const& initialLineState;
	std::unique_ptr<SearchReplaceEngine> const& search;
	std::unique_ptr<Path> const& path;
	std::shared_ptr<StatusSink> const& statusSink;
	std::shared_ptr<NotificationSink> const& notificationSink;
	std::shared_ptr<InteractionSink> const& interactionSink;
	std::shared_ptr<SingleChoiceInteractionSink> const& singleChoiceInteractionSink;
	std::shared_ptr<FileDialogService> const& fileDialogService;
	std::shared_ptr<VideoSourceRequestService> const& videoSourceRequestService;
	std::shared_ptr<BackgroundRunnerFactory> const& backgroundRunnerFactory;

	explicit ConstContextCoreSession(Context const& context);
};

struct ContextUiSession {
	wxWindow *&parent;
	wxWindow *&previousFocus;
	wxWindow *&videoSlider;
	AudioBox *&audioBox;
	AudioKaraoke *&karaoke;
	BaseGrid *&subsGrid;
	std::unique_ptr<DialogManager>& dialog;
	FrameMain *&frame;
	VideoDisplay *&videoDisplay;

	explicit ContextUiSession(Context& context);
};

struct ConstContextUiSession {
	wxWindow *const& parent;
	wxWindow *const& previousFocus;
	wxWindow *const& videoSlider;
	AudioBox *const& audioBox;
	AudioKaraoke *const& karaoke;
	BaseGrid *const& subsGrid;
	std::unique_ptr<DialogManager> const& dialog;
	FrameMain *const& frame;
	VideoDisplay *const& videoDisplay;

	explicit ConstContextUiSession(Context const& context);
};

struct Context {
	// Note: order here matters quite a bit, as things need to be set up and
	// torn down in the correct order
	std::unique_ptr<AssFile> ass;
	std::unique_ptr<TextSelectionController> textSelectionController;
	std::unique_ptr<SubsController> subsController;
	std::unique_ptr<Project> project;
	std::unique_ptr<Automation4::ScriptManager> local_scripts;
	std::unique_ptr<SelectionController> selectionController;
	std::unique_ptr<VideoController> videoController;
	std::unique_ptr<AudioController> audioController;
	std::unique_ptr<InitialLineState> initialLineState;
	std::unique_ptr<SearchReplaceEngine> search;
	std::unique_ptr<Path> path;
	std::shared_ptr<StatusSink> statusSink;
	std::shared_ptr<NotificationSink> notificationSink;
	std::shared_ptr<InteractionSink> interactionSink;
	std::shared_ptr<SingleChoiceInteractionSink> singleChoiceInteractionSink;
	std::shared_ptr<FileDialogService> fileDialogService;
	std::shared_ptr<VideoSourceRequestService> videoSourceRequestService;
	std::shared_ptr<BackgroundRunnerFactory> backgroundRunnerFactory;

	// Things that should probably be in some sort of UI-context-model
	wxWindow *parent = nullptr;
	wxWindow *previousFocus = nullptr;
	wxWindow *videoSlider = nullptr;

	// Views (i.e. things that should eventually not be here at all)
	AudioBox *audioBox = nullptr;
	AudioKaraoke *karaoke = nullptr;
	BaseGrid *subsGrid = nullptr;
	std::unique_ptr<DialogManager> dialog;
	FrameMain *frame = nullptr;
	VideoDisplay *videoDisplay = nullptr;

	Context();
	~Context();

	std::shared_ptr<StatusSink> GetStatusSink() const;
	void ShowStatus(std::string const& message, int timeout_ms = 10000) const;
	std::shared_ptr<NotificationSink> GetNotificationSink() const;
	void ShowInfo(std::string const& message, std::string const& title = "Information") const;
	void ShowError(std::string const& message, std::string const& title = "Error") const;
	void ShowWarning(std::string const& message, std::string const& title = "Warning") const;
	std::shared_ptr<InteractionSink> GetInteractionSink() const;
	InteractionResult RequestInteraction(InteractionRequest const& request) const;
	std::shared_ptr<SingleChoiceInteractionSink> GetSingleChoiceInteractionSink() const;
	std::optional<int> RequestSingleChoice(SingleChoiceInteractionRequest const& request) const;
	std::shared_ptr<FileDialogService> GetFileDialogService() const;
	agi::fs::path RequestOpenFile(OpenFileDialogRequest const& request) const;
	std::vector<agi::fs::path> RequestOpenFiles(OpenFilesDialogRequest const& request) const;
	agi::fs::path RequestSaveFile(SaveFileDialogRequest const& request) const;
	agi::fs::path RequestSelectDirectory(SelectDirectoryDialogRequest const& request) const;
	std::shared_ptr<VideoSourceRequestService> GetVideoSourceRequestService() const;
	std::string RequestDummyVideoPath() const;
	std::unique_ptr<BackgroundRunner> CreateBackgroundRunner(std::string const& title = "", std::string const& message = "") const;

	// Returned on demand to avoid making Context subobject construction depend
	// on the bridge members themselves having already been initialized.
	ContextCoreSession GetCore() { return ContextCoreSession(*this); }
	ConstContextCoreSession GetCore() const { return ConstContextCoreSession(*this); }
	ContextUiSession GetUI() { return ContextUiSession(*this); }
	ConstContextUiSession GetUI() const { return ConstContextUiSession(*this); }
};

}
