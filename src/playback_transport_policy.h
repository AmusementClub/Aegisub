#pragma once

#include "navigation_preview_policy.h"
#include "operation_token.h"

#include <chrono>
#include <optional>
#include <vector>

class PlaybackTransportPolicy final {
public:
	using Clock = std::chrono::steady_clock;
	using TimePoint = Clock::time_point;

	enum class InteractionKind {
		Navigation,
		DragEdit,
		StepRepeat
	};

	enum class InputKind {
		PlayToggle,
		StopPlayback,
		PreviewMotion,
		PreviewRelease,
		InspectionStep,
		CancelPreview,
		Timer
	};

	struct Input {
		InputKind kind = InputKind::PreviewMotion;
		InteractionKind interaction = InteractionKind::Navigation;
		int target = 0;
		bool force = false;

		friend bool operator==(Input const& lhs, Input const& rhs) = default;
	};

	enum class OutputKind {
		StartPlayback,
		StopPlayback,
		Preview,
		Commit,
		Inspection
	};

	struct Output {
		OutputKind kind = OutputKind::Preview;
		InteractionKind interaction = InteractionKind::Navigation;
		int target = 0;
		OperationTokenSource::Token token;

		friend bool operator==(Output const& lhs, Output const& rhs) = default;
	};

	explicit PlaybackTransportPolicy(std::chrono::milliseconds preview_min_interval = std::chrono::milliseconds(33));
	~PlaybackTransportPolicy();

	PlaybackTransportPolicy(PlaybackTransportPolicy const&) = delete;
	PlaybackTransportPolicy& operator=(PlaybackTransportPolicy const&) = delete;
	PlaybackTransportPolicy(PlaybackTransportPolicy&&) = delete;
	PlaybackTransportPolicy& operator=(PlaybackTransportPolicy&&) = delete;

	std::vector<Output> Apply(Input input, TimePoint now);

	std::optional<TimePoint> NextPreviewTime() const;

	bool IsPlaying() const { return is_playing; }

private:
	bool is_playing = false;

	bool has_active_preview = false;
	InteractionKind active_preview_interaction = InteractionKind::Navigation;

	NavigationPreviewPolicy preview_policy;
	OperationTokenSource playback_token_source;
	OperationTokenSource inspection_token_source;

	Output MapPreviewOutput(InteractionKind interaction, NavigationPreviewPolicy::Output const& output) const;
	Output MakePlaybackOutput(OutputKind kind);
	Output MakeInspectionOutput(InteractionKind interaction, int target);
	void CancelPreviewSession();
};

