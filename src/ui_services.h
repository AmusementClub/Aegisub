#pragma once

#include <libaegisub/background_runner.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace agi {

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
		switch (request.buttons) {
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
};

class BackgroundRunnerFactory {
public:
	virtual ~BackgroundRunnerFactory() = default;
	virtual std::unique_ptr<BackgroundRunner> Create(std::string const& title, std::string const& message) = 0;
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
