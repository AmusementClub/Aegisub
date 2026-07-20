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

#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"

#include "ass_file.h"
#include "audio_controller.h"
#include "auto4_base.h"
#include "dialog_manager.h"
#include "initial_line_state.h"
#include "options.h"
#include "project.h"
#include "search_replace_engine.h"
#include "selection_controller.h"
#include "status_sink.h"
#include "subs_controller.h"
#include "text_selection_controller.h"
#include "ui_services.h"
#include "video_controller.h"

#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>

namespace agi {
ContextUiState::ContextUiState()
: dialog(make_unique<DialogManager>()) {
}

ContextUiState::~ContextUiState() = default;

ContextUiSession::ContextUiSession(ContextUiState& state)
: parent(state.parent)
, previousFocus(state.previousFocus)
, videoSlider(state.videoSlider)
, audioBox(state.audioBox)
, karaoke(state.karaoke)
, subsGrid(state.subsGrid)
, subsEditBox(state.subsEditBox)
, dialog(state.dialog)
, frame(state.frame)
, videoDisplay(state.videoDisplay)
, secondarySubtitleSession(state.secondarySubtitleSession) {
}

ConstContextUiSession::ConstContextUiSession(ContextUiState const& state)
: parent(state.parent)
, previousFocus(state.previousFocus)
, videoSlider(state.videoSlider)
, audioBox(state.audioBox)
, karaoke(state.karaoke)
, subsGrid(state.subsGrid)
, subsEditBox(state.subsEditBox)
, dialog(state.dialog)
, frame(state.frame)
, videoDisplay(state.videoDisplay)
, secondarySubtitleSession(state.secondarySubtitleSession) {
}

Context::Context()
: ass(make_unique<AssFile>())
, textSelectionController(make_unique<TextSelectionController>())
, subsController(make_unique<SubsController>(this))
, project(make_unique<Project>(this))
, local_scripts(make_unique<Automation4::LocalScriptManager>(this))
, selectionController(make_unique<SelectionController>(this))
, videoController(make_unique<VideoController>(this))
, audioController(make_unique<AudioController>(this))
, initialLineState(make_unique<InitialLineState>(this))
, search(make_unique<SearchReplaceEngine>(this))
, path(make_unique<Path>(*config::path))
, statusSink(std::make_shared<NullStatusSink>())
, notificationSink(std::make_shared<NullNotificationSink>())
, interactionSink(std::make_shared<NullInteractionSink>())
, singleChoiceInteractionSink(std::make_shared<NullSingleChoiceInteractionSink>())
, fileDialogService(std::make_shared<NullFileDialogService>())
, videoSourceRequestService(std::make_shared<NullVideoSourceRequestService>())
, backgroundRunnerFactory(std::make_shared<InlineBackgroundRunnerFactory>())
, projectUiStateSink(std::make_shared<NullProjectUiStateSink>())
, audioPlayerFactoryService(std::make_shared<NullAudioPlayerFactoryService>())
, automationBackgroundScriptRunnerFactory(std::make_shared<Automation4::NullAutomationBackgroundScriptRunnerFactory>())
, ui(make_unique<ContextUiState>())
{
	subsController->SetSelectionController(selectionController.get());
}

Context::~Context() = default;

std::shared_ptr<StatusSink> Context::GetStatusSink() const {
	return statusSink;
}

void Context::ShowStatus(std::string const& message, int timeout_ms) const {
	if (statusSink)
		statusSink->ShowStatus(message, timeout_ms);
}

std::shared_ptr<NotificationSink> Context::GetNotificationSink() const {
	return notificationSink;
}

void Context::ShowInfo(std::string const& message, std::string const& title) const {
	if (notificationSink)
		notificationSink->ShowInfo(title, message);
}

void Context::ShowError(std::string const& message, std::string const& title) const {
	if (notificationSink)
		notificationSink->ShowError(title, message);
}

void Context::ShowWarning(std::string const& message, std::string const& title) const {
	if (notificationSink)
		notificationSink->ShowWarning(title, message);
}

std::shared_ptr<InteractionSink> Context::GetInteractionSink() const {
	return interactionSink;
}

InteractionResult Context::RequestInteraction(InteractionRequest const& request) const {
	if (interactionSink)
		return interactionSink->Request(request);
	return InteractionResult::Cancel;
}

std::shared_ptr<SingleChoiceInteractionSink> Context::GetSingleChoiceInteractionSink() const {
	return singleChoiceInteractionSink;
}

std::optional<int> Context::RequestSingleChoice(SingleChoiceInteractionRequest const& request) const {
	if (singleChoiceInteractionSink)
		return singleChoiceInteractionSink->RequestSingleChoice(request);
	return std::nullopt;
}

std::shared_ptr<FileDialogService> Context::GetFileDialogService() const {
	return fileDialogService;
}

agi::fs::path Context::RequestOpenFile(OpenFileDialogRequest const& request) const {
	if (fileDialogService)
		return fileDialogService->RequestOpenFile(request);
	return {};
}

std::vector<agi::fs::path> Context::RequestOpenFiles(OpenFilesDialogRequest const& request) const {
	if (fileDialogService)
		return fileDialogService->RequestOpenFiles(request);
	return {};
}

agi::fs::path Context::RequestSaveFile(SaveFileDialogRequest const& request) const {
	if (fileDialogService)
		return fileDialogService->RequestSaveFile(request);
	return {};
}

agi::fs::path Context::RequestSelectDirectory(SelectDirectoryDialogRequest const& request) const {
	if (fileDialogService)
		return fileDialogService->RequestSelectDirectory(request);
	return {};
}

std::shared_ptr<VideoSourceRequestService> Context::GetVideoSourceRequestService() const {
	return videoSourceRequestService;
}

std::string Context::RequestDummyVideoPath() const {
	if (videoSourceRequestService)
		return videoSourceRequestService->RequestDummyVideoPath();
	return {};
}

std::unique_ptr<BackgroundRunner> Context::CreateBackgroundRunner(std::string const& title, std::string const& message) const {
	if (backgroundRunnerFactory)
		return backgroundRunnerFactory->Create(title, message);
	return std::make_unique<detail::InlineBackgroundRunner>();
}

std::shared_ptr<ProjectUiStateSink> Context::GetProjectUiStateSink() const {
	return projectUiStateSink;
}

std::shared_ptr<AudioPlayerFactoryService> Context::GetAudioPlayerFactoryService() const {
	return audioPlayerFactoryService;
}

std::unique_ptr<Automation4::BackgroundScriptRunner> Context::CreateAutomationBackgroundScriptRunner(std::string const& title) const {
	if (automationBackgroundScriptRunnerFactory)
		return automationBackgroundScriptRunnerFactory->Create(title);
	return {};
}

ContextUiSession Context::GetUI() {
	return ContextUiSession(*ui);
}

ConstContextUiSession Context::GetUI() const {
	return ConstContextUiSession(*ui);
}
}
