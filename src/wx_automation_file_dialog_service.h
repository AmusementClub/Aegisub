#pragma once

#include "wx_file_dialog_services.h"

namespace Automation4 {

// This header is the explicit wx adapter seam for automation export paths
// which need a window-backed file dialog fallback when no injected service
// is available.
inline std::shared_ptr<agi::FileDialogService> ResolveAutomationFileDialogService(
	std::shared_ptr<agi::FileDialogService> preferred_service,
	wxWindow *parent = nullptr) {
	if (preferred_service)
		return preferred_service;
	return agi::MakeWindowFileDialogService(parent);
}

}
