#pragma once

#include "operation_token.h"

#include <chrono>
#include <optional>

class NavigationPreviewPolicy final {
public:
	using Clock = std::chrono::steady_clock;
	using TimePoint = Clock::time_point;

	enum class OutputKind {
		Preview,
		Commit
	};

	struct Output {
		OutputKind kind = OutputKind::Preview;
		int target = 0;
		OperationTokenSource::Token token;

		friend bool operator==(Output const& lhs, Output const& rhs) = default;
	};

	explicit NavigationPreviewPolicy(std::chrono::milliseconds min_interval = std::chrono::milliseconds(33));
	~NavigationPreviewPolicy();

	NavigationPreviewPolicy(NavigationPreviewPolicy const&) = delete;
	NavigationPreviewPolicy& operator=(NavigationPreviewPolicy const&) = delete;
	NavigationPreviewPolicy(NavigationPreviewPolicy&&) = delete;
	NavigationPreviewPolicy& operator=(NavigationPreviewPolicy&&) = delete;

	/// Start a gesture, optionally treating the initial press as a click until
	/// motion exceeds drag_threshold in target units. Release always commits.
	void BeginGesture(bool defer_preview, int threshold = 0);

	/// Register a new navigation preview target.
	///
	/// - A deferred initial press waits for motion beyond the drag threshold.
	/// - Otherwise, `force` emits a Preview output immediately.
	/// - Otherwise, emits at most once per `min_interval`, coalescing to the latest target.
	std::optional<Output> OnMotion(int target, TimePoint now, bool force);

	/// Flush the final target as a Commit output and cancel any pending Preview output.
	Output OnRelease(int target, TimePoint now);

	/// Emit a pending Preview output once its deadline is reached.
	std::optional<Output> OnTimer(TimePoint now);

	/// Time at which a pending Preview output becomes eligible to emit.
	std::optional<TimePoint> NextPreviewTime() const;

	/// Cancel any pending Preview output and supersede previously issued tokens.
	void Cancel();

private:
	std::chrono::milliseconds min_interval;
	OperationTokenSource token_source;

	bool has_latest_target = false;
	int latest_target = 0;
	bool defer_initial_preview = false;
	int drag_threshold = 0;

	bool has_last_emit = false;
	TimePoint last_emit_time{};

	std::optional<int> pending_target;
	std::optional<TimePoint> pending_deadline;

	void ClearPending();
	Output Emit(OutputKind kind, int target, TimePoint now);
	bool ShouldEmitNow(TimePoint now, bool force) const;
};
