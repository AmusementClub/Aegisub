#pragma once

#include "provider_catalog_builder.h"
#include "provider_selection_diagnostics.h"

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace aegisub::provider_catalog {

enum class AudioProviderOpenAttemptFailure {
	Unavailable,
	FileNotFound,
	AudioNotFound,
	ProviderError,
};

enum class AudioProviderOpenFailure {
	FileNotFound,
	AudioNotFound,
	ProviderError,
};

struct AudioProviderOpenFailureReport {
	bool found_file = false;
	bool found_audio = false;
	std::string all_errors;
	std::string partial_errors;
};

enum class VideoProviderOpenAttemptFailure {
	Unavailable,
	FileNotFound,
	NotSupported,
	OpenError,
};

enum class VideoProviderOpenFailure {
	FileNotFound,
	NotSupported,
	OpenError,
};

struct VideoProviderOpenFailureReport {
	bool found = false;
	bool supported = false;
	std::string errors;
};

enum class ProviderOpenAttemptState {
	Unavailable,
	ReturnedNull,
	Opened,
};

template<typename ProviderPtr>
struct ProviderOpenAttemptResult {
	ProviderOpenAttemptState state = ProviderOpenAttemptState::Unavailable;
	ProviderPtr provider;
	std::string unavailable_reason;
};

void RecordAttempt(provider_selection_diagnostics::SelectionReport& report,
                   char const* provider_name,
                   char const* outcome,
                   std::string detail = {});
void RecordProviderReturnedNull(provider_selection_diagnostics::SelectionReport& report,
                                char const* provider_name);
void RecordProviderOpenSuccess(provider_selection_diagnostics::SelectionReport& report,
                               char const* provider_name);

template<typename Factory, typename Describe, typename Open>
auto TryOpenProviderFactory(Factory const& factory,
                            Describe describe,
                            provider_selection_diagnostics::SelectionReport& diagnostics,
                            Open&& open)
	-> ProviderOpenAttemptResult<std::decay_t<decltype(std::declval<Open>()(factory))>> {
	using ProviderPtr = std::decay_t<decltype(std::declval<Open>()(factory))>;

	ProviderOpenAttemptResult<ProviderPtr> result;
	auto descriptor = describe(factory);
	if (auto unavailable_reason = ProviderUnavailableReason(descriptor)) {
		result.state = ProviderOpenAttemptState::Unavailable;
		result.unavailable_reason = std::move(*unavailable_reason);
		return result;
	}

	auto provider = std::forward<Open>(open)(factory);
	if (!provider) {
		result.state = ProviderOpenAttemptState::ReturnedNull;
		RecordProviderReturnedNull(diagnostics, descriptor.name);
		return result;
	}

	result.state = ProviderOpenAttemptState::Opened;
	RecordProviderOpenSuccess(diagnostics, descriptor.name);
	result.provider = std::move(provider);
	return result;
}

std::string FormatAttemptErrorLine(char const* provider_name, std::string_view detail);
void AppendAttemptErrorLine(std::string& errors, char const* provider_name, std::string_view detail);
void RecordAudioProviderOpenFailure(AudioProviderOpenFailureReport& report,
                                    char const* provider_name,
                                    std::string_view detail,
                                    AudioProviderOpenAttemptFailure failure);
char const* AudioProviderOpenAttemptOutcome(AudioProviderOpenAttemptFailure failure);
void RecordAudioProviderOpenFailureAttempt(AudioProviderOpenFailureReport& report,
                                           provider_selection_diagnostics::SelectionReport& diagnostics,
                                           char const* provider_name,
                                           std::string_view error_detail,
                                           AudioProviderOpenAttemptFailure failure,
                                           char const* diagnostic_outcome = nullptr,
                                           std::string diagnostic_detail = {});
AudioProviderOpenFailure FinalAudioProviderOpenFailure(AudioProviderOpenFailureReport const& report);
std::string const& FinalAudioProviderOpenErrorDetail(AudioProviderOpenFailureReport const& report);
void RecordVideoProviderOpenFailure(VideoProviderOpenFailureReport& report,
                                    char const* provider_name,
                                    std::string_view detail,
                                    VideoProviderOpenAttemptFailure failure);
char const* VideoProviderOpenAttemptOutcome(VideoProviderOpenAttemptFailure failure);
void RecordVideoProviderOpenFailureAttempt(VideoProviderOpenFailureReport& report,
                                           provider_selection_diagnostics::SelectionReport& diagnostics,
                                           char const* provider_name,
                                           std::string_view detail,
                                           VideoProviderOpenAttemptFailure failure);
VideoProviderOpenFailure FinalVideoProviderOpenFailure(VideoProviderOpenFailureReport const& report);

} // namespace aegisub::provider_catalog
