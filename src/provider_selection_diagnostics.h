#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace aegisub::provider_selection_diagnostics {

struct Attempt {
	std::string provider_name;
	std::string outcome;
	std::string detail;
};

struct SelectionReport {
	std::string preferred_provider;
	std::string selected_provider;
	std::vector<Attempt> attempts;
};

inline std::string NormalizeProviderToken(std::string_view value) {
	std::string normalized;
	normalized.reserve(value.size());
	for (unsigned char ch : value) {
		if (std::isalnum(ch))
			normalized.push_back(static_cast<char>(std::tolower(ch)));
	}
	return normalized;
}

inline std::string CanonicalizeProviderName(std::string_view value) {
	if (value.empty())
		return {};

	auto const normalized = NormalizeProviderToken(value);
	if (normalized == "ffms2" || normalized == "ffmpegsource" || normalized == "ffmpegsource2")
		return "FFmpegSource";
	if (normalized == "lsmas" || normalized == "lsmasnative" || normalized == "lsmashworks" || normalized == "lwlibavsource")
		return "LsmasNative";
	if (normalized == "avisynth" || normalized == "avs")
		return "Avisynth";
	if (normalized == "yuv4mpeg" || normalized == "y4m")
		return "YUV4MPEG";
	if (normalized == "dummy")
		return "Dummy";
	if (normalized == "pcm")
		return "PCM";
	return std::string(value);
}

inline bool SameProviderName(std::string_view left, std::string_view right) {
	return !left.empty()
		&& !right.empty()
		&& CanonicalizeProviderName(left) == CanonicalizeProviderName(right);
}

inline bool UsedFallback(SelectionReport const& report) {
	return !report.preferred_provider.empty()
		&& !report.selected_provider.empty()
		&& !SameProviderName(report.preferred_provider, report.selected_provider);
}

inline std::string SanitizeText(std::string_view value) {
	std::string sanitized;
	sanitized.reserve(value.size());

	bool last_was_space = false;
	for (char ch : value) {
		if (ch == '\r' || ch == '\n' || ch == '\t') {
			if (!last_was_space && !sanitized.empty()) {
				sanitized.push_back(' ');
				last_was_space = true;
			}
			continue;
		}
		if (ch == '|')
			ch = '/';

		sanitized.push_back(ch);
		last_was_space = ch == ' ';
	}

	return sanitized;
}

inline std::string FormatAttempts(SelectionReport const& report) {
	std::string text;
	for (auto const& attempt : report.attempts) {
		if (!text.empty())
			text += " | ";

		text += attempt.provider_name;
		text += ":";
		text += attempt.outcome;

		auto const detail = SanitizeText(attempt.detail);
		if (!detail.empty()) {
			text += " (";
			text += detail;
			text += ")";
		}
	}
	return text;
}

inline std::string DescribeFallbackReason(SelectionReport const& report) {
	if (!UsedFallback(report))
		return {};

	for (auto const& attempt : report.attempts) {
		if (!SameProviderName(attempt.provider_name, report.preferred_provider) || attempt.outcome == "opened")
			continue;

		auto const detail = SanitizeText(attempt.detail);
		if (detail.empty())
			return attempt.outcome;
		return attempt.outcome + ": " + detail;
	}

	if (!report.selected_provider.empty())
		return "preferred provider did not open; used " + report.selected_provider;
	return "preferred provider did not open";
}

} // namespace aegisub::provider_selection_diagnostics
