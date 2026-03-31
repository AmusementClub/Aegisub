#pragma once

#include "auto4_base.h"
#include "dialog_progress.h"
#include "include/aegisub/audio_player.h"
#include "status_sink.h"
#include "ui_dispatch.h"
#include "ui_services.h"
#include "wx_frame_main_request_host.h"

#include <functional>

namespace agi {

// This header is the explicit wx host seam for frame_main runtime-backed UI
// capabilities such as progress dialogs, project UI state restore, audio
// player creation, and automation background runners.

class WxFrameMainBackgroundRunner final : public BackgroundRunner {
	wxWindow *parent = nullptr;
	ui::WeakLifetime lifetime;
	std::string title;
	std::string message;

public:
	WxFrameMainBackgroundRunner(wxWindow *parent, ui::WeakLifetime lifetime, std::string title, std::string message)
	: parent(parent)
	, lifetime(std::move(lifetime))
	, title(std::move(title))
	, message(std::move(message)) {
	}

	void Run(std::function<void(ProgressSink *)> task) override {
		ui::MainInvoke([this, task = std::move(task)]() mutable {
			if (!lifetime.lock()) {
				detail::InlineBackgroundRunner fallback;
				fallback.Run(std::move(task));
				return;
			}

			DialogProgress dialog(parent, to_wx(title), to_wx(message));
			dialog.Run(std::move(task));
		});
	}
};

class WxFrameMainBackgroundRunnerFactory final : public BackgroundRunnerFactory {
	wxWindow *parent = nullptr;
	ui::WeakLifetime lifetime;

public:
	WxFrameMainBackgroundRunnerFactory(wxWindow *parent, ui::WeakLifetime lifetime)
	: parent(parent)
	, lifetime(std::move(lifetime)) {
	}

	std::unique_ptr<BackgroundRunner> Create(std::string const& title, std::string const& message) override {
		return std::make_unique<WxFrameMainBackgroundRunner>(parent, lifetime, title, message);
	}
};

class WxFrameMainStatusSink final : public StatusSink {
	std::function<void(std::string const&, int)> show_status;
	ui::WeakLifetime lifetime;

public:
	WxFrameMainStatusSink(std::function<void(std::string const&, int)> show_status, ui::WeakLifetime lifetime)
	: show_status(std::move(show_status))
	, lifetime(std::move(lifetime)) {
	}

	void ShowStatus(std::string const& message, int timeout_ms) override {
		ui::MainAsyncIfAlive(lifetime, [show_status = show_status, message, timeout_ms] {
			show_status(message, timeout_ms);
		});
	}
};

class WxFrameMainProjectUiStateSink final : public ProjectUiStateSink {
	std::function<void(ProjectUiStateSnapshot const&)> restore;
	ui::WeakLifetime lifetime;

public:
	WxFrameMainProjectUiStateSink(std::function<void(ProjectUiStateSnapshot const&)> restore, ui::WeakLifetime lifetime)
	: restore(std::move(restore))
	, lifetime(std::move(lifetime)) {
	}

	void RestoreProjectUiState(ProjectUiStateSnapshot const& state) override {
		ui::MainInvokeIfAlive(lifetime, [restore = restore, state] {
			restore(state);
		});
	}
};

class WxFrameMainAudioPlayerFactoryService final : public AudioPlayerFactoryService {
	wxWindow *parent = nullptr;
	ui::WeakLifetime lifetime;

public:
	WxFrameMainAudioPlayerFactoryService(wxWindow *parent, ui::WeakLifetime lifetime)
	: parent(parent)
	, lifetime(std::move(lifetime)) {
	}

	std::unique_ptr<AudioPlayer> CreateAudioPlayer(AudioProvider *provider) override {
		return ui::MainInvoke([parent = parent, lifetime = lifetime, provider] {
			if (!lifetime.lock())
				return std::unique_ptr<AudioPlayer>();
			AudioPlayerHost host;
			host.native_parent_handle = parent ? reinterpret_cast<void *>(parent->GetHandle()) : nullptr;
			return AudioPlayerFactory::GetAudioPlayer(provider, host);
		});
	}
};

class WxFrameMainAutomationBackgroundScriptRunnerFactory final : public Automation4::AutomationBackgroundScriptRunnerFactory {
	wxWindow *parent = nullptr;
	ui::WeakLifetime lifetime;

public:
	WxFrameMainAutomationBackgroundScriptRunnerFactory(wxWindow *parent, ui::WeakLifetime lifetime)
	: parent(parent)
	, lifetime(std::move(lifetime)) {
	}

	std::unique_ptr<Automation4::BackgroundScriptRunner> Create(std::string const& title) override {
		return ui::MainInvoke([parent = parent, lifetime = lifetime, title] {
			if (!lifetime.lock())
				return std::unique_ptr<Automation4::BackgroundScriptRunner>();
			return std::make_unique<Automation4::BackgroundScriptRunner>(
				parent,
				title,
				MakeFrameMainFileDialogService(parent, lifetime));
		});
	}
};

inline std::shared_ptr<BackgroundRunnerFactory> MakeFrameMainBackgroundRunnerFactory(wxWindow *parent, ui::WeakLifetime lifetime) {
	return std::make_shared<WxFrameMainBackgroundRunnerFactory>(parent, std::move(lifetime));
}

inline std::shared_ptr<StatusSink> MakeFrameMainStatusSink(
	std::function<void(std::string const&, int)> show_status,
	ui::WeakLifetime lifetime) {
	return std::make_shared<WxFrameMainStatusSink>(std::move(show_status), std::move(lifetime));
}

inline std::shared_ptr<ProjectUiStateSink> MakeFrameMainProjectUiStateSink(
	std::function<void(ProjectUiStateSnapshot const&)> restore,
	ui::WeakLifetime lifetime) {
	return std::make_shared<WxFrameMainProjectUiStateSink>(std::move(restore), std::move(lifetime));
}

inline std::shared_ptr<AudioPlayerFactoryService> MakeFrameMainAudioPlayerFactoryService(wxWindow *parent, ui::WeakLifetime lifetime) {
	return std::make_shared<WxFrameMainAudioPlayerFactoryService>(parent, std::move(lifetime));
}

inline std::shared_ptr<Automation4::AutomationBackgroundScriptRunnerFactory> MakeFrameMainAutomationBackgroundScriptRunnerFactory(
	wxWindow *parent,
	ui::WeakLifetime lifetime) {
	return std::make_shared<WxFrameMainAutomationBackgroundScriptRunnerFactory>(parent, std::move(lifetime));
}

}
