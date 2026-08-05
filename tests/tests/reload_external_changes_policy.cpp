#include <main.h>

#include "../../src/reload_external_changes_policy.h"
#include "../../src/ui_timer.h"
#include "../../src/watched_file.h"

#include <libaegisub/fs.h>
#include <libaegisub/option.h>
#include <libaegisub/option_value.h>
#include <libaegisub/signal.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::filesystem::path ProjectRoot() {
	return std::filesystem::path(AEGISUB_PROJECT_SOURCE_DIR).lexically_normal();
}

std::string ReadFileText(std::filesystem::path const& path) {
	std::ifstream input(path, std::ios::binary);
	std::ostringstream out;
	out << input.rdbuf();
	return out.str();
}

/// Minimal JSON-ish check: "Reload External Changes" : true (allowing spaces).
bool OptionDefaultsToTrue(std::string const& json) {
	auto key = json.find("\"Reload External Changes\"");
	if (key == std::string::npos)
		return false;
	auto colon = json.find(':', key);
	if (colon == std::string::npos)
		return false;
	auto value = json.find_first_not_of(" \t\r\n", colon + 1);
	if (value == std::string::npos)
		return false;
	return json.compare(value, 4, "true") == 0;
}

class RecordingUiTimer final : public UiTimer {
	std::function<void()> callback;

public:
	int start_once_delay_ms = -1;
	int stop_count = 0;
	bool running = false;

	explicit RecordingUiTimer(std::function<void()> callback)
	: callback(std::move(callback)) {
	}

	void StartOnce(int delay_ms) override {
		start_once_delay_ms = delay_ms;
		running = true;
	}

	void StartRepeating(int) override {
	}

	void Stop() override {
		++stop_count;
		running = false;
	}

	bool IsRunning() const override {
		return running;
	}

	void Fire() {
		running = false;
		if (callback)
			callback();
	}
};

class RecordingUiTimerHost final : public UiTimerHost {
public:
	RecordingUiTimer *last_timer = nullptr;

	std::unique_ptr<UiTimer> CreateTimer(std::function<void()> callback) override {
		auto timer = std::make_unique<RecordingUiTimer>(std::move(callback));
		last_timer = timer.get();
		return timer;
	}
};

class ScopedInstalledUiTimerHost {
	std::shared_ptr<UiTimerHost> previous;

public:
	explicit ScopedInstalledUiTimerHost(std::shared_ptr<UiTimerHost> host)
	: previous(GetUiTimerHost()) {
		InstallUiTimerHost(std::move(host));
	}

	~ScopedInstalledUiTimerHost() {
		InstallUiTimerHost(std::move(previous));
	}
};

class FakeFileSystemWatcherBackend final : public FileSystemWatcherBackend {
public:
	agi::fs::path watched_directory;
	FileSystemWatcherListener *listener = nullptr;
	int watch_count = 0;
	int reset_count = 0;

	bool WatchDirectory(agi::fs::path const& directory, FileSystemWatcherListener* new_listener) override {
		++watch_count;
		watched_directory = directory;
		listener = new_listener;
		return true;
	}

	void Reset() override {
		++reset_count;
		watched_directory.clear();
		listener = nullptr;
	}

	void Emit(FileSystemWatchEvent const& event) {
		if (listener)
			listener->OnFileSystemWatchEvent(event);
	}
};

/// Applies PlanWatchArm to a real WatchedFile the same way SubsController does
/// (create / bind / disarm-without-destroy). Decision logic comes only from
/// PlanWatchArm (shared with production); this object only performs the side
/// effects on WatchedFile.
class FakeWatchSession {
public:
	std::unique_ptr<WatchedFile> file_watch;
	FakeFileSystemWatcherBackend *backend = nullptr;
	bool has_baseline = false;
	bool pending = false;
	int changed_callbacks = 0;
	/// When true, the next changed callback disarms via ApplyOption(false) on
	/// the same stack (nested option-off during timer Fire).
	bool disarm_inside_changed_callback = false;
	agi::fs::path last_path;

	void ApplyOption(bool option_enabled, bool has_open_file, agi::fs::path const& path) {
		using reload_external_changes::PlanWatchArm;
		last_path = path;
		auto const plan = PlanWatchArm(
			option_enabled,
			static_cast<bool>(file_watch),
			has_open_file,
			has_baseline);

		if (plan.disarm_watcher && file_watch)
			file_watch->ClearTargetPath();
		if (plan.clear_pending)
			pending = false;
		if (plan.clear_snapshots)
			has_baseline = false;

		if (plan.create_watcher) {
			auto owned = std::make_unique<FakeFileSystemWatcherBackend>();
			backend = owned.get();
			file_watch = std::make_unique<WatchedFile>(std::move(owned));
			file_watch->SetChangedCallback([this](agi::fs::path const& changed_path) {
				++changed_callbacks;
				if (disarm_inside_changed_callback) {
					// Nested disarm while this callable is still on the stack —
					// must not destroy WatchedFile (production uses ClearTargetPath only).
					ApplyOption(false, true, changed_path.empty() ? last_path : changed_path);
				}
			});
		}

		if (plan.bind_target && file_watch)
			file_watch->SetTargetPath(path);
		if (plan.record_baseline_if_missing)
			has_baseline = true;
	}
};

} // namespace

// --- Headless / GUI tracking gate ---

TEST(reload_external_changes_policy, snapshots_are_gui_shell_only) {
	using reload_external_changes::ShouldTrackExternalFileSnapshots;
	EXPECT_FALSE(ShouldTrackExternalFileSnapshots(false));
	EXPECT_TRUE(ShouldTrackExternalFileSnapshots(true));
}

TEST(reload_external_changes_policy, overwrite_check_requires_enabled_detection) {
	using reload_external_changes::ShouldCheckExternalFileSnapshot;

	EXPECT_TRUE(ShouldCheckExternalFileSnapshot(true, true, true, true));
	EXPECT_FALSE(ShouldCheckExternalFileSnapshot(true, false, true, true));
	EXPECT_FALSE(ShouldCheckExternalFileSnapshot(false, true, true, true));
	EXPECT_FALSE(ShouldCheckExternalFileSnapshot(true, true, false, true));
	EXPECT_FALSE(ShouldCheckExternalFileSnapshot(true, true, true, false));
}

// --- Arm / disarm policy ---

TEST(reload_external_changes_policy, disable_disarms_without_destroy_intent) {
	using reload_external_changes::PlanWatchArm;

	auto const plan = PlanWatchArm(false, true, true, true);
	EXPECT_TRUE(plan.disarm_watcher);
	EXPECT_FALSE(plan.create_watcher);
	EXPECT_FALSE(plan.bind_target);
	EXPECT_FALSE(plan.record_baseline_if_missing);
	EXPECT_TRUE(plan.clear_pending);
	EXPECT_TRUE(plan.clear_snapshots);
}

TEST(reload_external_changes_policy, enable_with_existing_baseline_does_not_rebaseline) {
	using reload_external_changes::PlanWatchArm;

	auto const already_armed = PlanWatchArm(true, true, true, true);
	EXPECT_FALSE(already_armed.create_watcher);
	EXPECT_TRUE(already_armed.bind_target);
	EXPECT_FALSE(already_armed.record_baseline_if_missing);
	EXPECT_FALSE(already_armed.clear_snapshots);

	auto const reenable = PlanWatchArm(true, false, true, true);
	EXPECT_TRUE(reenable.create_watcher);
	EXPECT_TRUE(reenable.bind_target);
	EXPECT_FALSE(reenable.record_baseline_if_missing);
	EXPECT_FALSE(reenable.clear_snapshots);
}

TEST(reload_external_changes_policy, enable_without_baseline_records_once) {
	using reload_external_changes::PlanWatchArm;
	auto const plan = PlanWatchArm(true, false, true, false);
	EXPECT_TRUE(plan.create_watcher);
	EXPECT_TRUE(plan.bind_target);
	EXPECT_TRUE(plan.record_baseline_if_missing);
	EXPECT_FALSE(plan.clear_snapshots);
}

TEST(reload_external_changes_policy, after_prompt_yes_always_reloads) {
	using reload_external_changes::AfterPromptAction;
	using reload_external_changes::PlanAfterPrompt;
	// Explicit Yes is honored even if detection was turned off mid-prompt.
	EXPECT_EQ(AfterPromptAction::Reload, PlanAfterPrompt(true, false, false));
	EXPECT_EQ(AfterPromptAction::Reload, PlanAfterPrompt(true, false, true));
	EXPECT_EQ(AfterPromptAction::Reload, PlanAfterPrompt(true, true, false));
	EXPECT_EQ(AfterPromptAction::StampPromptedAndContinue, PlanAfterPrompt(false, true, true));
	EXPECT_EQ(AfterPromptAction::StampPromptedAndStop, PlanAfterPrompt(false, false, true));
}

// --- WatchedFile integration: enable / disable / re-enable / no self-destroy ---

TEST(reload_external_changes_policy, fake_session_enable_disable_keeps_watcher_object) {
	auto timer_host = std::make_shared<RecordingUiTimerHost>();
	ScopedInstalledUiTimerHost timer_scope(timer_host);

	FakeWatchSession session;
	auto const path = agi::fs::PathFromString("project/subs/external.ass");

	session.ApplyOption(true, true, path);
	ASSERT_NE(session.file_watch, nullptr);
	ASSERT_NE(session.backend, nullptr);
	EXPECT_EQ(1, session.backend->watch_count);
	EXPECT_TRUE(session.has_baseline);
	EXPECT_FALSE(session.file_watch->GetTargetPath().empty());

	// Disable: disarm only — object and backend pointer remain valid.
	auto *alive_backend = session.backend;
	int const resets_before = alive_backend->reset_count;
	session.ApplyOption(false, true, path);
	ASSERT_NE(session.file_watch, nullptr);
	EXPECT_EQ(alive_backend, session.backend);
	EXPECT_GT(alive_backend->reset_count, resets_before);
	EXPECT_TRUE(session.file_watch->GetTargetPath().empty());
	EXPECT_FALSE(session.has_baseline) << "disabled detection must forget its disk baseline";

	// Re-enable with existing object: bind again and record a fresh baseline.
	session.ApplyOption(true, true, path);
	EXPECT_EQ(alive_backend, session.backend);
	EXPECT_FALSE(session.file_watch->GetTargetPath().empty());
	EXPECT_GE(alive_backend->watch_count, 2);
	EXPECT_TRUE(session.has_baseline);
}

TEST(reload_external_changes_policy, disarm_inside_changed_callback_is_safe) {
	// Nested option-off must run on the changed-callback stack (timer Fire),
	// not merely cancel a pending timer before Fire.
	auto timer_host = std::make_shared<RecordingUiTimerHost>();
	ScopedInstalledUiTimerHost timer_scope(timer_host);

	FakeWatchSession session;
	session.disarm_inside_changed_callback = true;
	auto const path = agi::fs::PathFromString("project/subs/external.ass");
	session.ApplyOption(true, true, path);
	ASSERT_NE(session.backend, nullptr);
	ASSERT_NE(timer_host->last_timer, nullptr);
	auto *alive_backend = session.backend;
	auto *alive_watch = session.file_watch.get();

	FileSystemWatchEvent event;
	event.kind = FileSystemWatchEventKind::Modified;
	event.path = path;
	session.backend->Emit(event);
	ASSERT_TRUE(timer_host->last_timer->running);

	// Fire runs changed_callback, which calls ApplyOption(false) nested.
	timer_host->last_timer->Fire();

	EXPECT_EQ(1, session.changed_callbacks);
	EXPECT_EQ(alive_watch, session.file_watch.get()) << "watcher object must not be destroyed in-callback";
	EXPECT_EQ(alive_backend, session.backend);
	EXPECT_TRUE(session.file_watch->GetTargetPath().empty());
	EXPECT_FALSE(session.has_baseline);
}

TEST(reload_external_changes_policy, redundant_enable_notify_does_not_drop_baseline_flag) {
	auto timer_host = std::make_shared<RecordingUiTimerHost>();
	ScopedInstalledUiTimerHost timer_scope(timer_host);

	FakeWatchSession session;
	auto const path = agi::fs::PathFromString("project/subs/external.ass");
	session.ApplyOption(true, true, path);
	EXPECT_TRUE(session.has_baseline);
	// Simulate Preferences Apply re-notifying true with baseline already set.
	session.ApplyOption(true, true, path);
	EXPECT_TRUE(session.has_baseline);
	EXPECT_NE(session.file_watch, nullptr);
}

// --- Connection lifetime ---

TEST(reload_external_changes_policy, scoped_option_subscription_disconnects) {
	static constexpr char kDefaults[] = R"json({
		"App": {
			"Auto": {
				"Reload External Changes": true
			}
		}
	})json";

	agi::Options options("", kDefaults, agi::Options::FLUSH_SKIP);
	int calls = 0;
	{
		agi::signal::Connection connection = options.Get("App/Auto/Reload External Changes")->Subscribe(
			[&](agi::OptionValue const&) { ++calls; });
		options.Get("App/Auto/Reload External Changes")->SetBool(false);
		EXPECT_EQ(1, calls);
		connection.Disconnect();
		options.Get("App/Auto/Reload External Changes")->SetBool(true);
		EXPECT_EQ(1, calls);
	}
}

// --- Default config value (precise key→value, not any "true" in the file) ---

TEST(reload_external_changes_policy, default_config_enables_option) {
	for (auto const& rel : {
		std::filesystem::path("src/libresrc/default_config.json"),
		std::filesystem::path("src/libresrc/osx/default_config.json"),
	}) {
		auto const text = ReadFileText(ProjectRoot() / rel);
		EXPECT_TRUE(OptionDefaultsToTrue(text)) << rel;
	}
}
