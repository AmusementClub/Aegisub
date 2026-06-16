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

namespace agi {

ContextCoreSession::ContextCoreSession(Context& context)
: ass(context.ass)
, textSelectionController(context.textSelectionController)
, subsController(context.subsController)
, project(context.project)
, local_scripts(context.local_scripts)
, selectionController(context.selectionController)
, videoController(context.videoController)
, audioController(context.audioController)
, initialLineState(context.initialLineState)
, search(context.search)
, path(context.path)
, statusSink(context.statusSink)
, notificationSink(context.notificationSink)
, interactionSink(context.interactionSink)
, singleChoiceInteractionSink(context.singleChoiceInteractionSink)
, fileDialogService(context.fileDialogService)
, videoSourceRequestService(context.videoSourceRequestService)
, backgroundRunnerFactory(context.backgroundRunnerFactory)
, projectUiStateSink(context.projectUiStateSink)
, audioPlayerFactoryService(context.audioPlayerFactoryService)
, automationBackgroundScriptRunnerFactory(context.automationBackgroundScriptRunnerFactory) {
}

ConstContextCoreSession::ConstContextCoreSession(Context const& context)
: ass(context.ass)
, textSelectionController(context.textSelectionController)
, subsController(context.subsController)
, project(context.project)
, local_scripts(context.local_scripts)
, selectionController(context.selectionController)
, videoController(context.videoController)
, audioController(context.audioController)
, initialLineState(context.initialLineState)
, search(context.search)
, path(context.path)
, statusSink(context.statusSink)
, notificationSink(context.notificationSink)
, interactionSink(context.interactionSink)
, singleChoiceInteractionSink(context.singleChoiceInteractionSink)
, fileDialogService(context.fileDialogService)
, videoSourceRequestService(context.videoSourceRequestService)
, backgroundRunnerFactory(context.backgroundRunnerFactory)
, projectUiStateSink(context.projectUiStateSink)
, audioPlayerFactoryService(context.audioPlayerFactoryService)
, automationBackgroundScriptRunnerFactory(context.automationBackgroundScriptRunnerFactory) {
}

}
