#pragma once

#include "compat.h"
#include "ui_dispatch.h"
#include "ui_services.h"
#include "utils.h"

#include <algorithm>
#include <wx/dirdlg.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>
#include <wx/window.h>

namespace agi {

inline int ToWxMessageBoxFlags(InteractionButtons buttons, InteractionIcon icon) {
	int flags = 0;
	switch (buttons) {
	case InteractionButtons::Ok:
		flags |= wxOK;
		break;
	case InteractionButtons::OkCancel:
		flags |= wxOK | wxCANCEL;
		break;
	case InteractionButtons::YesNo:
		flags |= wxYES_NO;
		break;
	case InteractionButtons::YesNoCancel:
		flags |= wxYES_NO | wxCANCEL;
		break;
	}

	switch (icon) {
	case InteractionIcon::None:
		break;
	case InteractionIcon::Info:
		flags |= wxICON_INFORMATION;
		break;
	case InteractionIcon::Warning:
		flags |= wxICON_WARNING;
		break;
	case InteractionIcon::Error:
		flags |= wxICON_ERROR;
		break;
	case InteractionIcon::Question:
		flags |= wxICON_QUESTION;
		break;
	}

	return flags | wxCENTER;
}

inline InteractionResult FromWxMessageBoxResult(int result) {
	switch (result) {
	case wxOK:
		return InteractionResult::Ok;
	case wxCANCEL:
		return InteractionResult::Cancel;
	case wxYES:
		return InteractionResult::Yes;
	case wxNO:
		return InteractionResult::No;
	default:
		return InteractionResult::Cancel;
	}
}

inline std::optional<int> ShowSingleChoiceDialog(wxWindow *parent, SingleChoiceInteractionRequest const& request) {
	if (request.choices.empty())
		return std::nullopt;

	wxDialog dialog(parent, -1, to_wx(request.title));

	auto sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(new wxStaticText(&dialog, -1, to_wx(request.message)), wxSizerFlags().Border());

	auto choices = to_wx(request.choices);
	auto *radio_box = new wxRadioBox(&dialog, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, choices, 1);
	radio_box->SetSelection(std::clamp(request.default_choice, 0, static_cast<int>(request.choices.size() - 1)));
	sizer->Add(radio_box, wxSizerFlags().Border(wxALL & ~wxTOP).Expand());

	sizer->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), wxSizerFlags().Border().Expand());

	dialog.SetSizerAndFit(sizer);
	dialog.CenterOnParent();

	dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dialog.EndModal(wxID_OK); }, wxID_OK);
	dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); }, wxID_CANCEL);

	return dialog.ShowModal() == wxID_OK ? std::optional<int>(radio_box->GetSelection()) : std::nullopt;
}

class WxMessageBoxNotificationSink final : public NotificationSink {
	wxWindow *parent = nullptr;

	void Show(std::string const& title, std::string const& message, int flags) {
		agi::ui::MainInvoke([parent = parent, title, message, flags] {
			wxMessageBox(to_wx(message), to_wx(title), flags | wxCENTER, parent);
		});
	}

public:
	explicit WxMessageBoxNotificationSink(wxWindow *parent = nullptr)
	: parent(parent) {
	}

	void ShowInfo(std::string const& title, std::string const& message) override {
		Show(title, message, wxOK | wxICON_INFORMATION);
	}

	void ShowError(std::string const& title, std::string const& message) override {
		Show(title, message, wxOK | wxICON_ERROR);
	}

	void ShowWarning(std::string const& title, std::string const& message) override {
		Show(title, message, wxOK | wxICON_WARNING);
	}
};

class WxMessageBoxInteractionSink final : public InteractionSink {
	wxWindow *parent = nullptr;

public:
	explicit WxMessageBoxInteractionSink(wxWindow *parent = nullptr)
	: parent(parent) {
	}

	InteractionResult Request(InteractionRequest const& request) override {
		return agi::ui::MainInvoke([parent = parent, request] {
			return FromWxMessageBoxResult(wxMessageBox(
				to_wx(request.message),
				to_wx(request.title),
				ToWxMessageBoxFlags(request.buttons, request.icon),
				parent));
		});
	}
};

class WxSingleChoiceInteractionSink final : public SingleChoiceInteractionSink {
	wxWindow *parent = nullptr;

public:
	explicit WxSingleChoiceInteractionSink(wxWindow *parent = nullptr)
	: parent(parent) {
	}

	std::optional<int> RequestSingleChoice(SingleChoiceInteractionRequest const& request) override {
		return agi::ui::MainInvoke([parent = parent, request] {
			bool was_busy = wxIsBusy();
			if (was_busy)
				wxEndBusyCursor();

			auto result = ShowSingleChoiceDialog(parent, request);

			if (was_busy)
				wxBeginBusyCursor();
			return result;
		});
	}
};

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
				parent);
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
				parent);
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
				parent);
		});
	}

	agi::fs::path RequestSelectDirectory(SelectDirectoryDialogRequest const& request) override {
		return agi::ui::MainInvoke([parent = parent, request] {
			return SelectDirectorySelector(to_wx(request.title), request.default_path, parent);
		});
	}
};

inline std::shared_ptr<NotificationSink> MakeWindowNotificationSink(wxWindow *parent = nullptr) {
	return std::make_shared<WxMessageBoxNotificationSink>(parent);
}

inline std::shared_ptr<InteractionSink> MakeWindowInteractionSink(wxWindow *parent = nullptr) {
	return std::make_shared<WxMessageBoxInteractionSink>(parent);
}

inline std::shared_ptr<SingleChoiceInteractionSink> MakeWindowSingleChoiceInteractionSink(wxWindow *parent = nullptr) {
	return std::make_shared<WxSingleChoiceInteractionSink>(parent);
}

inline std::shared_ptr<FileDialogService> MakeWindowFileDialogService(wxWindow *parent = nullptr) {
	return std::make_shared<WxWindowFileDialogService>(parent);
}

}
