#include "navigation_preview_policy.h"

#include <algorithm>
#include <utility>

NavigationPreviewPolicy::NavigationPreviewPolicy(std::chrono::milliseconds min_interval)
: min_interval(min_interval) {
}

NavigationPreviewPolicy::~NavigationPreviewPolicy() = default;

void NavigationPreviewPolicy::BeginGesture(bool defer_preview, int threshold) {
	Cancel();
	has_latest_target = false;
	has_last_emit = false;
	defer_initial_preview = defer_preview;
	drag_threshold = std::max(0, threshold);
}

void NavigationPreviewPolicy::ClearPending() {
	pending_target.reset();
	pending_deadline.reset();
}

bool NavigationPreviewPolicy::ShouldEmitNow(TimePoint now, bool force) const {
	if (force)
		return true;
	if (!has_last_emit)
		return true;
	return now - last_emit_time >= min_interval;
}

NavigationPreviewPolicy::Output NavigationPreviewPolicy::Emit(OutputKind kind, int target, TimePoint now) {
	has_last_emit = true;
	last_emit_time = now;
	return Output{ kind, target, token_source.Issue() };
}

std::optional<NavigationPreviewPolicy::Output> NavigationPreviewPolicy::OnMotion(int target, TimePoint now, bool force) {
	if (defer_initial_preview) {
		if (!has_latest_target) {
			has_latest_target = true;
			latest_target = target;
			return std::nullopt;
		}
		auto const delta = static_cast<long long>(target) - latest_target;
		if (delta >= -drag_threshold && delta <= drag_threshold) {
			return std::nullopt;
		}
		defer_initial_preview = false;
	}

	if (!has_latest_target) {
		has_latest_target = true;
		latest_target = target;
	}
	else if (target != latest_target) {
		latest_target = target;
		token_source.Supersede();
	}
	else if (!force) {
		return std::nullopt;
	}

	if (ShouldEmitNow(now, force)) {
		ClearPending();
		return Emit(OutputKind::Preview, latest_target, now);
	}

	pending_target = latest_target;
	pending_deadline = last_emit_time + min_interval;
	return std::nullopt;
}

NavigationPreviewPolicy::Output NavigationPreviewPolicy::OnRelease(int target, TimePoint now) {
	defer_initial_preview = false;
	if (!has_latest_target) {
		has_latest_target = true;
		latest_target = target;
	}
	else {
		latest_target = target;
	}

	token_source.Supersede();
	ClearPending();
	return Emit(OutputKind::Commit, latest_target, now);
}

std::optional<NavigationPreviewPolicy::Output> NavigationPreviewPolicy::OnTimer(TimePoint now) {
	if (!pending_target || !pending_deadline)
		return std::nullopt;
	if (now < *pending_deadline)
		return std::nullopt;

	int const target = *pending_target;
	ClearPending();
	return Emit(OutputKind::Preview, target, now);
}

std::optional<NavigationPreviewPolicy::TimePoint> NavigationPreviewPolicy::NextPreviewTime() const {
	return pending_deadline;
}

void NavigationPreviewPolicy::Cancel() {
	defer_initial_preview = false;
	token_source.Supersede();
	ClearPending();
}
