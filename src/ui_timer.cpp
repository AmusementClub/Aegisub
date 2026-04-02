#include "ui_timer.h"

#include <mutex>

namespace {

std::mutex& UiTimerHostMutex() {
	static std::mutex mutex;
	return mutex;
}

std::shared_ptr<UiTimerHost>& InstalledUiTimerHost() {
	static std::shared_ptr<UiTimerHost> host;
	return host;
}

}

void InstallUiTimerHost(std::shared_ptr<UiTimerHost> host) {
	std::lock_guard<std::mutex> lock(UiTimerHostMutex());
	InstalledUiTimerHost() = std::move(host);
}

std::shared_ptr<UiTimerHost> GetUiTimerHost() {
	std::lock_guard<std::mutex> lock(UiTimerHostMutex());
	return InstalledUiTimerHost();
}

void ResetUiTimerHost() {
	std::lock_guard<std::mutex> lock(UiTimerHostMutex());
	InstalledUiTimerHost().reset();
}
