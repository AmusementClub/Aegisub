#include <aegisub/fontcollector/fontcollector.h>

#include <array>
#include <cstring>

namespace {

bool Contains(char const *text, char const *needle) {
	return text && needle && std::strstr(text, needle);
}

} // namespace

int main() {
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

	return 0;
}
