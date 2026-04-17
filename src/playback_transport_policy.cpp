#include "playback_transport_policy.h"

PlaybackTransportPolicy::PlaybackTransportPolicy(std::chrono::milliseconds preview_min_interval)
: preview_policy(preview_min_interval) {
}

PlaybackTransportPolicy::~PlaybackTransportPolicy() = default;

PlaybackTransportPolicy::Output PlaybackTransportPolicy::MapPreviewOutput(
	InteractionKind interaction,
	NavigationPreviewPolicy::Output const& output) const
{
	auto kind = OutputKind::Preview;
	if (output.kind == NavigationPreviewPolicy::OutputKind::Commit)
		kind = OutputKind::Commit;
	return Output{ kind, interaction, output.target, output.token };
}

PlaybackTransportPolicy::Output PlaybackTransportPolicy::MakePlaybackOutput(OutputKind kind) {
	playback_token_source.Supersede();
	return Output{ kind, InteractionKind::Navigation, 0, playback_token_source.Issue() };
}

PlaybackTransportPolicy::Output PlaybackTransportPolicy::MakeInspectionOutput(InteractionKind interaction, int target) {
	inspection_token_source.Supersede();
	return Output{ OutputKind::Inspection, interaction, target, inspection_token_source.Issue() };
}

void PlaybackTransportPolicy::CancelPreviewSession() {
	if (!has_active_preview)
		return;
	preview_policy.Cancel();
	has_active_preview = false;
}

std::vector<PlaybackTransportPolicy::Output> PlaybackTransportPolicy::Apply(Input input, TimePoint now) {
	std::vector<Output> outputs;

	auto stop_playback_if_needed = [&] {
		if (!is_playing)
			return;
		is_playing = false;
		outputs.push_back(MakePlaybackOutput(OutputKind::StopPlayback));
	};

	switch (input.kind) {
		case InputKind::PlayToggle: {
			CancelPreviewSession();
			if (is_playing) {
				is_playing = false;
				outputs.push_back(MakePlaybackOutput(OutputKind::StopPlayback));
				break;
			}
			is_playing = true;
			outputs.push_back(MakePlaybackOutput(OutputKind::StartPlayback));
			break;
		}

		case InputKind::StopPlayback: {
			CancelPreviewSession();
			if (!is_playing)
				break;
			is_playing = false;
			outputs.push_back(MakePlaybackOutput(OutputKind::StopPlayback));
			break;
		}

		case InputKind::PreviewMotion: {
			stop_playback_if_needed();

			if (!has_active_preview || input.interaction != active_preview_interaction) {
				preview_policy.Cancel();
				has_active_preview = true;
				active_preview_interaction = input.interaction;
			}

			auto out = preview_policy.OnMotion(input.target, now, input.force);
			if (out)
				outputs.push_back(MapPreviewOutput(input.interaction, *out));
			break;
		}

		case InputKind::PreviewRelease: {
			stop_playback_if_needed();

			if (!has_active_preview || input.interaction != active_preview_interaction) {
				preview_policy.Cancel();
				has_active_preview = true;
				active_preview_interaction = input.interaction;
			}

			auto out = preview_policy.OnRelease(input.target, now);
			outputs.push_back(MapPreviewOutput(input.interaction, out));
			has_active_preview = false;
			break;
		}

		case InputKind::InspectionStep: {
			stop_playback_if_needed();
			CancelPreviewSession();
			outputs.push_back(MakeInspectionOutput(input.interaction, input.target));
			break;
		}

		case InputKind::CancelPreview: {
			CancelPreviewSession();
			break;
		}

		case InputKind::Timer: {
			if (!has_active_preview)
				break;
			auto out = preview_policy.OnTimer(now);
			if (out)
				outputs.push_back(MapPreviewOutput(active_preview_interaction, *out));
			break;
		}
	}

	return outputs;
}

std::optional<PlaybackTransportPolicy::TimePoint> PlaybackTransportPolicy::NextPreviewTime() const {
	if (!has_active_preview)
		return std::nullopt;
	return preview_policy.NextPreviewTime();
}

