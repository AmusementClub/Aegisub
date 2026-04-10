#pragma once

#include "dialogs.h"
#include "ui_dispatch.h"
#include "ui_services.h"
#include "wx_file_dialog_services.h"

#include <wx/dnd.h>

namespace agi {

// This header is the explicit wx host seam for frame_main request/selection
// flows such as window-backed file dialogs and dummy-video creation.

class WxFrameMainFileDialogService final : public FileDialogService {
	std::shared_ptr<FileDialogService> delegate;
	ui::WeakLifetime lifetime;

public:
	WxFrameMainFileDialogService(wxWindow *parent, ui::WeakLifetime lifetime)
	: delegate(MakeWindowFileDialogService(parent))
	, lifetime(std::move(lifetime)) {
	}

	agi::fs::path RequestOpenFile(OpenFileDialogRequest const& request) override {
		return ui::MainInvoke([delegate = delegate, lifetime = lifetime, request] {
			if (!lifetime.lock())
				return agi::fs::path();
			return delegate->RequestOpenFile(request);
		});
	}

	std::vector<agi::fs::path> RequestOpenFiles(OpenFilesDialogRequest const& request) override {
		return ui::MainInvoke([delegate = delegate, lifetime = lifetime, request] {
			if (!lifetime.lock())
				return std::vector<agi::fs::path>();
			return delegate->RequestOpenFiles(request);
		});
	}

	agi::fs::path RequestSaveFile(SaveFileDialogRequest const& request) override {
		return ui::MainInvoke([delegate = delegate, lifetime = lifetime, request] {
			if (!lifetime.lock())
				return agi::fs::path();
			return delegate->RequestSaveFile(request);
		});
	}

	agi::fs::path RequestSelectDirectory(SelectDirectoryDialogRequest const& request) override {
		return ui::MainInvoke([delegate = delegate, lifetime = lifetime, request] {
			if (!lifetime.lock())
				return agi::fs::path();
			return delegate->RequestSelectDirectory(request);
		});
	}
};

class WxFrameMainVideoSourceRequestService final : public VideoSourceRequestService {
	wxWindow *parent = nullptr;
	ui::WeakLifetime lifetime;

public:
	WxFrameMainVideoSourceRequestService(wxWindow *parent, ui::WeakLifetime lifetime)
	: parent(parent)
	, lifetime(std::move(lifetime)) {
	}

	std::string RequestDummyVideoPath() override {
		return ui::MainInvoke([parent = parent, lifetime = lifetime] {
			if (!lifetime.lock())
				return std::string();
			return CreateDummyVideo(parent);
		});
	}
};

class WxFrameMainFileDropTarget final : public wxFileDropTarget {
	std::function<void(std::vector<agi::fs::path> const&)> open_files;
	ui::WeakLifetime lifetime;

public:
	WxFrameMainFileDropTarget(std::function<void(std::vector<agi::fs::path> const&)> open_files, ui::WeakLifetime lifetime)
	: open_files(std::move(open_files))
	, lifetime(std::move(lifetime)) {
	}

	bool OnDropFiles(wxCoord, wxCoord, wxArrayString const& filenames) override {
		std::vector<agi::fs::path> files;
		files.reserve(filenames.size());
		for (wxString const& filename : filenames)
			files.push_back(agi::fs::PathFromString(from_wx(filename)));
		ui::MainAsyncIfAlive(lifetime, [open_files = open_files, files = std::move(files)] {
			open_files(files);
		});
		return true;
	}
};

inline std::shared_ptr<FileDialogService> MakeFrameMainFileDialogService(wxWindow *parent, ui::WeakLifetime lifetime) {
	return std::make_shared<WxFrameMainFileDialogService>(parent, std::move(lifetime));
}

inline std::shared_ptr<VideoSourceRequestService> MakeFrameMainVideoSourceRequestService(wxWindow *parent, ui::WeakLifetime lifetime) {
	return std::make_shared<WxFrameMainVideoSourceRequestService>(parent, std::move(lifetime));
}

inline wxFileDropTarget *MakeFrameMainFileDropTarget(
	std::function<void(std::vector<agi::fs::path> const&)> open_files,
	ui::WeakLifetime lifetime) {
	return new WxFrameMainFileDropTarget(std::move(open_files), std::move(lifetime));
}

}
