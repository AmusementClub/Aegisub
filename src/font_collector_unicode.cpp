// Copyright (c) 2026, MIRIMIRIM

#include "font_collector_unicode.h"

namespace {
using font_collector::unicode::Rune;

bool is_valid_scalar_value(uint32_t value) {
	return value <= 0x10FFFF && (value - 0xD800) > 0x7FF;
}

font_collector::unicode::OperationStatus invalid_sequence(Rune& result, int& bytes_consumed, int consumed) {
	result = Rune();
	bytes_consumed = consumed;
	return font_collector::unicode::OperationStatus::InvalidData;
}

font_collector::unicode::OperationStatus need_more_data(Rune& result, int& bytes_consumed, int consumed) {
	result = Rune();
	bytes_consumed = consumed;
	return font_collector::unicode::OperationStatus::NeedMoreData;
}

#ifdef _WIN32
wchar_t high_surrogate(Rune rune) {
	return static_cast<wchar_t>(0xD800 + ((rune.Value() - 0x10000) >> 10));
}

wchar_t low_surrogate(Rune rune) {
	return static_cast<wchar_t>(0xDC00 + ((rune.Value() - 0x10000) & 0x3FF));
}
#endif
}

namespace font_collector::unicode {

int Rune::Utf8SequenceLength() const {
	if (value < 0x80)
		return 1;
	if (value < 0x800)
		return 2;
	if (value < 0x10000)
		return 3;
	return 4;
}

int Rune::Utf16SequenceLength() const {
	return value < 0x10000 ? 1 : 2;
}

bool Rune::TryEncodeToUtf8(std::span<char> destination, int& bytes_written) const {
	bytes_written = 0;
	auto const size = destination.size();
	if (size == 0)
		return false;

	auto const value = this->value;
	if (value < 0x80) {
		destination[0] = static_cast<char>(value);
		bytes_written = 1;
		return true;
	}
	if (size < 2)
		return false;

	if (value < 0x800) {
		destination[0] = static_cast<char>((value >> 6) | 0xC0);
		destination[1] = static_cast<char>((value & 0x3F) | 0x80);
		bytes_written = 2;
		return true;
	}
	if (size < 3)
		return false;

	if (value < 0x10000) {
		destination[0] = static_cast<char>((value >> 12) | 0xE0);
		destination[1] = static_cast<char>(((value >> 6) & 0x3F) | 0x80);
		destination[2] = static_cast<char>((value & 0x3F) | 0x80);
		bytes_written = 3;
		return true;
	}
	if (size < 4)
		return false;

	destination[0] = static_cast<char>((value >> 18) | 0xF0);
	destination[1] = static_cast<char>(((value >> 12) & 0x3F) | 0x80);
	destination[2] = static_cast<char>(((value >> 6) & 0x3F) | 0x80);
	destination[3] = static_cast<char>((value & 0x3F) | 0x80);
	bytes_written = 4;
	return true;
}

bool Rune::IsValid(uint32_t scalar_value) {
	return is_valid_scalar_value(scalar_value);
}

#ifdef _WIN32
bool Rune::TryEncodeToUtf16(std::span<wchar_t> destination, int& chars_written) const {
	chars_written = 0;
	if (destination.empty())
		return false;

	auto const value = this->value;
	if (value < 0x10000) {
		destination[0] = static_cast<wchar_t>(value);
		chars_written = 1;
		return true;
	}
	if (destination.size() < 2)
		return false;

	destination[0] = high_surrogate(*this);
	destination[1] = low_surrogate(*this);
	chars_written = 2;
	return true;
}
#endif

bool Rune::TryCreate(uint32_t scalar_value, Rune& result) {
	if (!IsValid(scalar_value)) {
		result = Rune();
		return false;
	}

	result = Rune(scalar_value);
	return true;
}

OperationStatus Rune::DecodeFromUtf8(char const *source, size_t length, Rune& result, int& bytes_consumed) {
	result = Rune();
	bytes_consumed = 0;

	if (length == 0)
		return need_more_data(result, bytes_consumed, 0);

	auto const lead = static_cast<unsigned char>(source[0]);
	if (lead < 0x80) {
		result = Rune(lead);
		bytes_consumed = 1;
		return OperationStatus::Done;
	}

	if (lead < 0xC2 || lead > 0xF4)
		return invalid_sequence(result, bytes_consumed, 1);

	if (length < 2)
		return need_more_data(result, bytes_consumed, 1);

	auto const second = static_cast<unsigned char>(source[1]);
	if ((second & 0xC0) != 0x80)
		return invalid_sequence(result, bytes_consumed, 1);

	if (lead < 0xE0) {
		result = Rune(((lead & 0x1F) << 6) | (second & 0x3F));
		bytes_consumed = 2;
		return OperationStatus::Done;
	}

	if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second >= 0xA0))
		return invalid_sequence(result, bytes_consumed, 1);

	if (lead < 0xF0) {
		if (length < 3)
			return need_more_data(result, bytes_consumed, 2);

		auto const third = static_cast<unsigned char>(source[2]);
		if ((third & 0xC0) != 0x80)
			return invalid_sequence(result, bytes_consumed, 2);

		result = Rune(((lead & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F));
		bytes_consumed = 3;
		return OperationStatus::Done;
	}

	if ((lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second > 0x8F))
		return invalid_sequence(result, bytes_consumed, 1);

	if (length < 3)
		return need_more_data(result, bytes_consumed, 2);

	auto const third = static_cast<unsigned char>(source[2]);
	if ((third & 0xC0) != 0x80)
		return invalid_sequence(result, bytes_consumed, 2);

	if (length < 4)
		return need_more_data(result, bytes_consumed, 3);

	auto const fourth = static_cast<unsigned char>(source[3]);
	if ((fourth & 0xC0) != 0x80)
		return invalid_sequence(result, bytes_consumed, 3);

	result = Rune(((lead & 0x07) << 18) | ((second & 0x3F) << 12) | ((third & 0x3F) << 6) | (fourth & 0x3F));
	bytes_consumed = 4;
	return OperationStatus::Done;
}

OperationStatus Rune::DecodeFromUtf8(std::string_view source, Rune& result, int& bytes_consumed) {
	return DecodeFromUtf8(source.data(), source.size(), result, bytes_consumed);
}

void AppendRuneToUtf8(std::string& out, Rune rune) {
	char buffer[4];
	int bytes_written = 0;
	if (rune.TryEncodeToUtf8(buffer, bytes_written))
		out.append(buffer, bytes_written);
}

#ifdef _WIN32
bool IsSurrogate(wchar_t ch) {
	return (static_cast<unsigned>(ch) - 0xD800) <= 0x7FF;
}

bool IsHighSurrogate(wchar_t ch) {
	return (static_cast<unsigned>(ch) - 0xD800) <= 0x3FF;
}

bool IsLowSurrogate(wchar_t ch) {
	return (static_cast<unsigned>(ch) - 0xDC00) <= 0x3FF;
}
#endif
}
