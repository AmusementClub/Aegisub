#include "fontcollector_cli_encoding.h"

#include <array>
#include <fstream>
#include <system_error>

namespace aegisub::fontcollector_cli {
namespace {

class Utf8Validator {
	int remaining = 0;
	unsigned char next_min = 0x80;
	unsigned char next_max = 0xbf;

public:
	bool Consume(char const* data, size_t size) {
		for (size_t index = 0; index < size; ++index) {
			auto const value = static_cast<unsigned char>(data[index]);
			if (remaining) {
				if (value < next_min || value > next_max)
					return false;
				--remaining;
				next_min = 0x80;
				next_max = 0xbf;
				continue;
			}
			if (value <= 0x7f)
				continue;
			if (value >= 0xc2 && value <= 0xdf) {
				remaining = 1;
				continue;
			}
			if (value >= 0xe0 && value <= 0xef) {
				remaining = 2;
				if (value == 0xe0)
					next_min = 0xa0;
				else if (value == 0xed)
					next_max = 0x9f;
				continue;
			}
			if (value >= 0xf0 && value <= 0xf4) {
				remaining = 3;
				if (value == 0xf0)
					next_min = 0x90;
				else if (value == 0xf4)
					next_max = 0x8f;
				continue;
			}
			return false;
		}
		return true;
	}

	bool Complete() const noexcept { return remaining == 0; }
};

}

std::string PreferredAutomaticEncoding(std::filesystem::path const& input) {
	std::error_code error;
	auto const file_size = std::filesystem::file_size(input, error);
	if (!error && file_size == 0)
		return "ascii";

	std::ifstream stream(input, std::ios::binary);
	if (!stream)
		return {};

	std::array<char, 65536> buffer = {};
	stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
	auto const size = static_cast<size_t>(stream.gcount());
	if (stream.bad())
		return {};
	auto byte = [&](size_t index) {
		return static_cast<unsigned char>(buffer[index]);
	};
	if (size >= 3 && byte(0) == 0xef && byte(1) == 0xbb && byte(2) == 0xbf)
		return "utf-8";
	if (size >= 4) {
		if (byte(0) == 0x00 && byte(1) == 0x00 && byte(2) == 0xfe && byte(3) == 0xff)
			return "utf-32be";
		if (byte(0) == 0xff && byte(1) == 0xfe && byte(2) == 0x00 && byte(3) == 0x00)
			return "utf-32le";
		if (byte(0) == 0x1a && byte(1) == 0x45 && byte(2) == 0xdf && byte(3) == 0xa3)
			return "binary";
	}
	if (size >= 2) {
		if (byte(0) == 0xfe && byte(1) == 0xff)
			return "utf-16be";
		if (byte(0) == 0xff && byte(1) == 0xfe)
			return "utf-16le";
	}

	size_t binaryish = 0;
	for (size_t index = 0; index < size; ++index) {
		auto const value = byte(index);
		if (value < 32 && value != '\r' && value != '\n' && value != '\t')
			++binaryish;
	}
	if (size && binaryish > size / 8)
		return {};

	Utf8Validator utf8;
	if (!utf8.Consume(buffer.data(), size))
		return {};
	while (stream) {
		stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
		auto const read = static_cast<size_t>(stream.gcount());
		if (stream.bad())
			return {};
		if (!read)
			break;
		if (!utf8.Consume(buffer.data(), read))
			return {};
	}
	return utf8.Complete() ? "utf-8" : std::string();
}

}
