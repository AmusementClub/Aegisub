#include "provider_open_policy.h"

#include <utility>

namespace aegisub::provider_catalog {

void RecordAttempt(provider_selection_diagnostics::SelectionReport& report,
                   char const* provider_name,
                   char const* outcome,
                   std::string detail) {
	report.attempts.push_back({provider_name ? provider_name : "", outcome ? outcome : "", std::move(detail)});
}

void RecordProviderReturnedNull(provider_selection_diagnostics::SelectionReport& report,
                                char const* provider_name) {
	RecordAttempt(report, provider_name, "returned_null", "provider factory returned null");
}

void RecordProviderOpenSuccess(provider_selection_diagnostics::SelectionReport& report,
                               char const* provider_name) {
	report.selected_provider = provider_name ? provider_name : "";
	RecordAttempt(report, provider_name, "opened");
}

std::string FormatAttemptErrorLine(char const* provider_name, std::string_view detail) {
	std::string line;
	line.append(provider_name ? provider_name : "");
	line.append(": ");
	line.append(detail);
	line.push_back('\n');
	return line;
}

void AppendAttemptErrorLine(std::string& errors, char const* provider_name, std::string_view detail) {
	errors.append(provider_name ? provider_name : "");
	errors.append(": ");
	errors.append(detail);
	errors.push_back('\n');
}

void RecordAudioProviderOpenFailure(AudioProviderOpenFailureReport& report,
                                    char const* provider_name,
                                    std::string_view detail,
                                    AudioProviderOpenAttemptFailure failure) {
	switch (failure) {
	case AudioProviderOpenAttemptFailure::Unavailable:
	case AudioProviderOpenAttemptFailure::FileNotFound:
		AppendAttemptErrorLine(report.all_errors, provider_name, detail);
		break;
	case AudioProviderOpenAttemptFailure::AudioNotFound:
		report.found_file = true;
		AppendAttemptErrorLine(report.all_errors, provider_name, detail);
		break;
	case AudioProviderOpenAttemptFailure::ProviderError:
		report.found_file = true;
		report.found_audio = true;
		{
			auto line = FormatAttemptErrorLine(provider_name, detail);
			report.all_errors.append(line);
			report.partial_errors.append(line);
			break;
		}
	}
}

char const* AudioProviderOpenAttemptOutcome(AudioProviderOpenAttemptFailure failure) {
	switch (failure) {
	case AudioProviderOpenAttemptFailure::Unavailable:
		return "unavailable";
	case AudioProviderOpenAttemptFailure::FileNotFound:
		return "file_not_found";
	case AudioProviderOpenAttemptFailure::AudioNotFound:
		return "no_audio";
	case AudioProviderOpenAttemptFailure::ProviderError:
		return "error";
	}
	return "error";
}

void RecordAudioProviderOpenFailureAttempt(AudioProviderOpenFailureReport& report,
                                           provider_selection_diagnostics::SelectionReport& diagnostics,
                                           char const* provider_name,
                                           std::string_view error_detail,
                                           AudioProviderOpenAttemptFailure failure,
                                           char const* diagnostic_outcome,
                                           std::string diagnostic_detail) {
	RecordAudioProviderOpenFailure(report, provider_name, error_detail, failure);
	if (diagnostic_detail.empty())
		diagnostic_detail = std::string(error_detail);
	RecordAttempt(diagnostics, provider_name, diagnostic_outcome ? diagnostic_outcome : AudioProviderOpenAttemptOutcome(failure), std::move(diagnostic_detail));
}

AudioProviderOpenFailure FinalAudioProviderOpenFailure(AudioProviderOpenFailureReport const& report) {
	if (report.found_audio)
		return AudioProviderOpenFailure::ProviderError;
	if (report.found_file)
		return AudioProviderOpenFailure::AudioNotFound;
	return AudioProviderOpenFailure::FileNotFound;
}

std::string const& FinalAudioProviderOpenErrorDetail(AudioProviderOpenFailureReport const& report) {
	if (FinalAudioProviderOpenFailure(report) == AudioProviderOpenFailure::ProviderError)
		return report.partial_errors;
	return report.all_errors;
}

void RecordVideoProviderOpenFailure(VideoProviderOpenFailureReport& report,
                                    char const* provider_name,
                                    std::string_view detail,
                                    VideoProviderOpenAttemptFailure failure) {
	switch (failure) {
	case VideoProviderOpenAttemptFailure::Unavailable:
	case VideoProviderOpenAttemptFailure::FileNotFound:
		break;
	case VideoProviderOpenAttemptFailure::NotSupported:
		report.found = true;
		break;
	case VideoProviderOpenAttemptFailure::OpenError:
		report.supported = true;
		break;
	}
	AppendAttemptErrorLine(report.errors, provider_name, detail);
}

char const* VideoProviderOpenAttemptOutcome(VideoProviderOpenAttemptFailure failure) {
	switch (failure) {
	case VideoProviderOpenAttemptFailure::Unavailable:
		return "unavailable";
	case VideoProviderOpenAttemptFailure::FileNotFound:
		return "file_not_found";
	case VideoProviderOpenAttemptFailure::NotSupported:
		return "not_supported";
	case VideoProviderOpenAttemptFailure::OpenError:
		return "error";
	}
	return "error";
}

void RecordVideoProviderOpenFailureAttempt(VideoProviderOpenFailureReport& report,
                                           provider_selection_diagnostics::SelectionReport& diagnostics,
                                           char const* provider_name,
                                           std::string_view detail,
                                           VideoProviderOpenAttemptFailure failure) {
	RecordVideoProviderOpenFailure(report, provider_name, detail, failure);
	RecordAttempt(diagnostics, provider_name, VideoProviderOpenAttemptOutcome(failure), std::string(detail));
}

VideoProviderOpenFailure FinalVideoProviderOpenFailure(VideoProviderOpenFailureReport const& report) {
	if (!report.found)
		return VideoProviderOpenFailure::FileNotFound;
	if (!report.supported)
		return VideoProviderOpenFailure::NotSupported;
	return VideoProviderOpenFailure::OpenError;
}

} // namespace aegisub::provider_catalog
