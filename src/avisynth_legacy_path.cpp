#include "avisynth_legacy_path.h"

#include <libaegisub/fs.h>

#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace {
#ifdef _WIN32
std::optional<std::string> TryConvertWidePathToAnsi(std::wstring const& path_text) {
	BOOL used_default_char = FALSE;
	int required = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, path_text.c_str(), -1, nullptr, 0, nullptr, &used_default_char);
	if (!required || used_default_char)
		return std::nullopt;

	std::vector<char> buffer(static_cast<size_t>(required));
	used_default_char = FALSE;
	int written = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, path_text.c_str(), -1, buffer.data(), required, nullptr, &used_default_char);
	if (!written || used_default_char)
		return std::nullopt;

	return std::string(buffer.data(), static_cast<size_t>(written - 1));
}

std::optional<std::wstring> TryGetShortPath(agi::fs::path const& path) {
	std::wstring buffer(MAX_PATH + 1, L'\0');
	DWORD len = GetShortPathNameW(path.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
	if (!len)
		return std::nullopt;

	while (len >= buffer.size()) {
		buffer.resize(len + 1, L'\0');
		len = GetShortPathNameW(path.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
		if (!len)
			return std::nullopt;
	}

	buffer.resize(len);
	return buffer;
}
#endif
}

namespace avisynth {
	std::optional<std::string> TryGetLegacyPathString(agi::fs::path const& path) {
#ifdef _WIN32
		if (auto direct_ansi_path = TryConvertWidePathToAnsi(path.native()))
			return direct_ansi_path;

		if (auto short_path = TryGetShortPath(path))
			return TryConvertWidePathToAnsi(*short_path);

		return std::nullopt;
#else
		return agi::fs::PathToString(path);
#endif
	}

	std::string BuildLegacyPathFailureMessage(char const *function_name, agi::fs::path const& path) {
		auto const function_label = function_name && *function_name ? std::string(function_name) : std::string("path API");
		auto const path_text = agi::fs::PathToString(path);
#ifdef _WIN32
		return "Aegisub: Avisynth " + function_label
			+ " fallback requires an ANSI or 8.3-safe path, but \""
			+ path_text
			+ "\" cannot be represented in the system code page and no short path is available.";
#else
		return "Aegisub: Avisynth " + function_label
			+ " fallback could not build a legacy path for \""
			+ path_text
			+ "\".";
#endif
	}
}