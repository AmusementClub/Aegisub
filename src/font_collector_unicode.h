// Copyright (c) 2026, MIRIMIRIM

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace font_collector::unicode {
enum class OperationStatus {
	Done = 0,
	DestinationTooSmall = 1,
	NeedMoreData = 2,
	InvalidData = 3
};

class Rune {
public:
	static constexpr uint32_t ReplacementCharValue = 0xFFFD;

	constexpr Rune() = default;

	uint32_t Value() const { return value; }
	int Utf8SequenceLength() const;
	int Utf16SequenceLength() const;
	bool TryEncodeToUtf8(std::span<char> destination, int& bytes_written) const;
#ifdef _WIN32
	bool TryEncodeToUtf16(std::span<wchar_t> destination, int& chars_written) const;
#endif

	static bool IsValid(uint32_t scalar_value);
	static bool TryCreate(uint32_t scalar_value, Rune& result);
	static OperationStatus DecodeFromUtf8(char const *source, size_t length, Rune& result, int& bytes_consumed);
	static OperationStatus DecodeFromUtf8(std::string_view source, Rune& result, int& bytes_consumed);

private:
	explicit constexpr Rune(uint32_t scalar_value) : value(scalar_value) { }

	uint32_t value = ReplacementCharValue;
};

void AppendRuneToUtf8(std::string& out, Rune rune);

#ifdef _WIN32
bool IsSurrogate(wchar_t ch);
bool IsHighSurrogate(wchar_t ch);
bool IsLowSurrogate(wchar_t ch);
#endif
}
