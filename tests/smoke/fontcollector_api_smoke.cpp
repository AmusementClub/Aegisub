#include <aegisub/fontcollector/fontcollector.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

bool Contains(char const *text, char const *needle) {
	return text && needle && std::strstr(text, needle);
}

} // namespace

int main() {
	static_assert(AEGISUB_FONTCOLLECTOR_MATCH_MEMORY_ONLY == 2);
	static_assert(AEGISUB_FONTCOLLECTOR_SESSION_OPTIONS_V1_SIZE <=
	              sizeof(AegisubFontCollectorSessionOptions));
	static_assert(AEGISUB_FONT_NAME_NORMALIZATION_REQUEST_V1_SIZE <=
	              sizeof(AegisubFontNameNormalizationRequest));
	static_assert(AEGISUB_FONT_NAME_NORMALIZATION_CHANGE_V1_SIZE <=
	              sizeof(AegisubFontNameNormalizationChange));
	static_assert(AEGISUB_FONT_NAME_NORMALIZATION_SUMMARY_V1_SIZE <=
	              sizeof(AegisubFontNameNormalizationSummary));
	static_assert(AEGISUB_FONT_NAME_NORMALIZATION_BATCH_ITEM_V1_SIZE <=
	              sizeof(AegisubFontNameNormalizationBatchItem));

	AegisubFontNameNormalizationRequest request = {};
	request.struct_size = AEGISUB_FONT_NAME_NORMALIZATION_REQUEST_V1_SIZE;
	request.input_path = "unused.ass";
	request.target = static_cast<AegisubFontNameNormalizationTarget>(99);

	AegisubFontNameNormalizationSummary summary = {};
	summary.struct_size = AEGISUB_FONT_NAME_NORMALIZATION_SUMMARY_V1_SIZE;
	std::array<char, 256> error = {};
	auto result = aegisub_fontcollector_build_normalization_plan(
		&request, nullptr, nullptr, &summary, error.data(), error.size());
	if (result != AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT ||
	    !Contains(error.data(), "invalid font-name normalization target"))
		return 1;
	if (summary.struct_size != AEGISUB_FONT_NAME_NORMALIZATION_SUMMARY_V1_SIZE)
		return 2;

	AegisubFontNameNormalizationBatchItem item = {};
	item.struct_size = AEGISUB_FONT_NAME_NORMALIZATION_BATCH_ITEM_V1_SIZE;
	item.request = &request;
	item.summary = &summary;
	item.error_buffer = error.data();
	item.error_buffer_size = error.size();
	AegisubFontNameNormalizationBatchItem *item_pointer = &item;
	std::array<char, 256> batch_error = {};
	result = aegisub_fontcollector_build_normalization_plan_batch(
		&item_pointer, 1, batch_error.data(), batch_error.size());
	if (result != AEGISUB_FONTCOLLECTOR_OK)
		return 3;
	if (item.result != AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT ||
	    !Contains(error.data(), "invalid font-name normalization target"))
		return 4;

	AegisubFontCollectorSessionOptions session_options = {};
	session_options.struct_size = sizeof(session_options);
	session_options.matcher = AEGISUB_FONTCOLLECTOR_MATCHER_PLATFORM;
	session_options.include_system_fonts = 0;
	AegisubFontCollectorSession *session = nullptr;
	result = aegisub_fontcollector_session_create_with_options(
		&session_options,
		nullptr,
		nullptr,
		&session,
		error.data(),
		error.size());
	if (result != AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT || session ||
	    !Contains(error.data(), "private font options require the libass matcher"))
		return 5;

	// Zero-init leaves include_system_fonts = 0; empty private libass catalog must fail closed.
	session_options = {};
	session_options.struct_size = sizeof(session_options);
	session_options.matcher = AEGISUB_FONTCOLLECTOR_MATCHER_LIBASS;
	session_options.include_system_fonts = 0;
	session = nullptr;
	error = {};
	result = aegisub_fontcollector_session_create_with_options(
		&session_options,
		nullptr,
		nullptr,
		&session,
		error.data(),
		error.size());
	if (result != AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT || session ||
	    !Contains(error.data(), "libass private catalog requires additional font files"))
		return 6;

	// Exercise the platform provider and the successful batch path with a
	// self-contained fixture. The file lives under the OS temporary directory so
	// the smoke does not depend on a checked-in machine-specific path.
	std::error_code fs_error;
	auto fixture_root = std::filesystem::temp_directory_path(fs_error);
	if (fs_error)
		return 7;
	fixture_root /= "aegisub-fontcollector-api-smoke-"
		+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
	if (!std::filesystem::create_directory(fixture_root, fs_error) || fs_error)
		return 8;
	struct FixtureCleanup {
		std::filesystem::path root;
		~FixtureCleanup() {
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	} cleanup{fixture_root};

	auto const input_path = fixture_root / "input.ass";
	{
		std::ofstream output(input_path, std::ios::binary);
		if (!output)
			return 9;
		output << "[Script Info]\n"
			          "ScriptType: v4.00+\n\n"
			          "[V4+ Styles]\n"
			          "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
			          "Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n\n"
			          "[Events]\n"
			          "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
			          "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,API smoke\n";
	}
	auto const encoded_path_value = input_path.generic_u8string();
	std::string encoded_path(encoded_path_value.begin(), encoded_path_value.end());

	session_options = {};
	session_options.struct_size = sizeof(session_options);
	session_options.matcher = AEGISUB_FONTCOLLECTOR_MATCHER_PLATFORM;
	session_options.include_system_fonts = 1;
	session = nullptr;
	error = {};
	result = aegisub_fontcollector_session_create_with_options(
		&session_options,
		nullptr,
		nullptr,
		&session,
		error.data(),
		error.size());
	if (result != AEGISUB_FONTCOLLECTOR_OK || !session)
		return 10;
	struct SessionCleanup {
		AegisubFontCollectorSession *session = nullptr;
		~SessionCleanup() {
			aegisub_fontcollector_session_destroy(session);
		}
	} session_cleanup{session};

	AegisubFontCollectorRequest collect_request = {};
	collect_request.input_path = encoded_path.c_str();
	collect_request.encoding = "UTF-8";
	collect_request.mode = AEGISUB_FONTCOLLECTOR_MODE_CHECK;
	AegisubFontCollectorSummary collect_summary = {};
	AegisubFontCollectorBatchItem collect_item = {};
	collect_item.request = collect_request;
	collect_item.summary = &collect_summary;
	std::array<char, 256> collect_error = {};
	collect_item.error_buffer = collect_error.data();
	collect_item.error_buffer_size = collect_error.size();
	result = aegisub_fontcollector_session_collect_batch(
		session,
		&collect_item,
		1,
		collect_error.data(),
		collect_error.size());
	if (result != AEGISUB_FONTCOLLECTOR_OK ||
	    collect_item.result != AEGISUB_FONTCOLLECTOR_OK)
		return 11;

	return 0;
}
