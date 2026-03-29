#pragma once

#include "compat.h"
#include "ui_dispatch.h"
#include "ui_services.h"
#include "utils.h"

#include <wx/window.h>

namespace agi {

// This header is the explicit wx adapter surface for window-backed file and
// directory dialogs. Replace it when the GUI shell no longer uses wx dialogs
// for path selection.

class WxWindowFileDialogService final : public FileDialogService {
	wxWindow *parent = nullptr;

public:
	explicit WxWindowFileDialogService(wxWindow *parent = nullptr)
	: parent(parent) {
	}

	agi::fs::path RequestOpenFile(OpenFileDialogRequest const& request) override {
		return agi::ui::MainInvoke([parent = parent, request] {
			return OpenFileSelector(
				to_wx(request.title),
				request.option_name,
				request.default_path,
				request.default_filename,
				request.default_extension,
				request.wildcard,
				parent,
				request.must_exist);
		});
	}

	std::vector<agi::fs::path> RequestOpenFiles(OpenFilesDialogRequest const& request) override {
		return agi::ui::MainInvoke([parent = parent, request] {
			return OpenFilesSelector(
				to_wx(request.title),
				request.option_name,
				request.default_path,
				request.default_filename,
				request.default_extension,
				request.wildcard,
				parent,
				request.must_exist);
		});
	}

	agi::fs::path RequestSaveFile(SaveFileDialogRequest const& request) override {
		return agi::ui::MainInvoke([parent = parent, request] {
			return SaveFileSelector(
				to_wx(request.title),
				request.option_name,
				request.default_path,
				request.default_filename,
				request.default_extension,
				request.wildcard,
				parent,
				request.prompt_overwrite);
		});
	}

	agi::fs::path RequestSelectDirectory(SelectDirectoryDialogRequest const& request) override {
		return agi::ui::MainInvoke([parent = parent, request] {
			return SelectDirectorySelector(to_wx(request.title), request.default_path, parent);
		});
	}
};

inline std::shared_ptr<FileDialogService> MakeWindowFileDialogService(wxWindow *parent = nullptr) {
	return std::make_shared<WxWindowFileDialogService>(parent);
}

}
