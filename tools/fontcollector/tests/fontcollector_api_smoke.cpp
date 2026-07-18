#include <aegisub/fontcollector/fontcollector.h>

#include <array>
#include <cstring>

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

	return 0;
}
