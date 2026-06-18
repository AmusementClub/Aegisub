#include "paste_over_policy.h"

namespace aegisub::paste_over_policy {

std::vector<bool> NormalizeFields(std::vector<bool> fields) {
	if (fields.size() == FieldCount - 1)
		fields.insert(fields.begin(), false);
	if (fields.size() < FieldCount)
		fields.resize(FieldCount, false);
	if (fields.size() > FieldCount)
		fields.resize(FieldCount);
	return fields;
}

std::vector<bool> BuildAllFields(bool checked) {
	return std::vector<bool>(FieldCount, checked);
}

std::vector<bool> BuildTimesFields() {
	auto fields = BuildAllFields(false);
	fields[StartTime] = true;
	fields[EndTime] = true;
	return fields;
}

std::vector<bool> BuildTextFields() {
	auto fields = BuildAllFields(false);
	fields[Text] = true;
	return fields;
}

} // namespace aegisub::paste_over_policy
