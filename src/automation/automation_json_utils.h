// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include <libaegisub/fs.h>

#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Automation4::json {
	class JsonObjectBuilder final {
		std::vector<std::pair<std::string, std::string>> fields;

	public:
		void AddRaw(std::string key, std::string value) {
			fields.emplace_back(std::move(key), std::move(value));
		}

		std::string Build() const {
			std::ostringstream out;
			out << "{";
			for (size_t i = 0; i < fields.size(); ++i) {
				if (i != 0)
					out << ", ";
				out << '"' << fields[i].first << "\": " << fields[i].second;
			}
			out << "}";
			return out.str();
		}
	};

	inline std::string JsonEscape(std::string const& input) {
		std::string escaped;
		escaped.reserve(input.size());
		for (unsigned char ch : input) {
			switch (ch) {
			case '\\': escaped += "\\\\"; break;
			case '"': escaped += "\\\""; break;
			case '\b': escaped += "\\b"; break;
			case '\f': escaped += "\\f"; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default:
				if (ch < 0x20) {
					char buffer[7];
					snprintf(buffer, sizeof(buffer), "\\u%04X", static_cast<unsigned>(ch));
					escaped += buffer;
				}
				else {
					escaped += static_cast<char>(ch);
				}
				break;
			}
		}
		return escaped;
	}

	inline std::string JsonString(std::string const& value) {
		return "\"" + JsonEscape(value) + "\"";
	}

	inline std::string JsonBool(bool value) {
		return value ? "true" : "false";
	}

	template<typename Integer>
	inline std::string JsonInteger(Integer value) {
		return std::to_string(value);
	}

	inline std::string JsonDouble(double value) {
		std::ostringstream out;
		out << value;
		return out.str();
	}

	inline std::string JsonNull() {
		return "null";
	}

	template<typename T, typename Serialize>
	inline void AddOptional(JsonObjectBuilder& builder, std::string key, std::optional<T> const& value, Serialize&& serialize) {
		if (value)
			builder.AddRaw(std::move(key), serialize(*value));
	}

	inline std::string SerializeIntArray(std::vector<int> const& values) {
		std::ostringstream out;
		out << "[";
		for (size_t i = 0; i < values.size(); ++i) {
			if (i != 0)
				out << ", ";
			out << values[i];
		}
		out << "]";
		return out.str();
	}

	inline std::string SerializeStringArray(std::vector<std::string> const& values) {
		std::ostringstream out;
		out << "[";
		for (size_t i = 0; i < values.size(); ++i) {
			if (i != 0)
				out << ", ";
			out << JsonString(values[i]);
		}
		out << "]";
		return out.str();
	}

	template<typename T, typename Serialize>
	inline std::string SerializeObjectArray(std::vector<T> const& values, Serialize&& serialize) {
		std::ostringstream out;
		out << "[";
		for (size_t i = 0; i < values.size(); ++i) {
			if (i != 0)
				out << ", ";
			out << serialize(values[i]);
		}
		out << "]";
		return out.str();
	}

	template<typename Range, typename Serialize>
	inline bool WriteJsonLinesFile(
		agi::fs::path const& path,
		Range const& values,
		Serialize&& serialize,
		std::string_view open_error,
		std::string& error) {
		agi::fs::CreateDirectory(path.parent_path());

		std::ofstream out(path, std::ios::out | std::ios::trunc);
		if (!out) {
			error = std::string(open_error);
			return false;
		}

		for (auto const& value : values)
			out << serialize(value) << "\n";

		return true;
	}
}
