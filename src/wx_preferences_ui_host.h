#pragma once

#include "wx_file_dialog_services.h"
#include "wx_message_box_ui_services.h"

namespace agi {

// This header is the explicit wx host seam for preferences-owned UI helper
// services such as file-dialog selection and reset-default interaction.

inline std::shared_ptr<FileDialogService> MakePreferencesFileDialogService(wxWindow *parent) {
	return MakeWindowFileDialogService(parent);
}

inline std::shared_ptr<InteractionSink> MakePreferencesInteractionSink(wxWindow *parent) {
	return MakeWindowInteractionSink(parent);
}

}
