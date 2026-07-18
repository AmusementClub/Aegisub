/**********************************************

License: BSD
Project Webpage: http://cajun-jsonapi.sourceforge.net/
Author: Terry Caton

***********************************************/

#include "libaegisub/cajun/reader.h"

#include <boost/interprocess/streams/bufferstream.hpp>
#include <cassert>
#include <cstdint>

/*

TODO:
* better documentation

*/

namespace json {
/// Wrapper around istream to keep track of document/line offsets
class Reader::InputStream {
	std::istream& m_iStr;
	Location m_Location;
public:
	InputStream(std::istream& iStr) : m_iStr(iStr) { }

	int Get() {
		assert(!m_iStr.eof());
		int c = m_iStr.get();

		++m_Location.m_nDocOffset;
		if (c == '\n') {
			++m_Location.m_nLine;
			m_Location.m_nLineOffset = 0;
		}
		else {
			++m_Location.m_nLineOffset;
		}

		return c;
	}

	int Peek() {
		assert(!m_iStr.eof());
		return m_iStr.peek();
	}

	bool EOS() {
		// libc++ doesn't set eof when a peek fails
		return m_iStr.peek() == EOF || m_iStr.eof();
	}

	Location const& GetLocation() const { return m_Location; }
};

class Reader::TokenStream {
	Tokens const& m_Tokens;
	Tokens::const_iterator m_itCurrent;

public:
	TokenStream(Tokens const& tokens) : m_Tokens(tokens), m_itCurrent(tokens.begin())
	{ }

	Token const& Peek() {
		assert(!EOS());
		return *m_itCurrent;
	}
	Token const& Get() {
		assert(!EOS());
		return *m_itCurrent++;
	}

	bool EOS() const { return m_itCurrent == m_Tokens.end(); }
};

void Reader::Read(UnknownElement& unknown, std::istream& istr) {
	Reader reader;

	Tokens tokens;
	InputStream inputStream(istr);
	reader.Scan(tokens, inputStream);

	TokenStream tokenStream(tokens);
	unknown = reader.Parse(tokenStream);

	if (!tokenStream.EOS()) {
		Token const& token = tokenStream.Peek();
		throw ParseException("Expected End of token stream; found " + token.sValue, token.locBegin, token.locEnd);
	}
}

void Reader::Scan(Tokens& tokens, InputStream& inputStream) {
	while (EatWhiteSpace(inputStream), !inputStream.EOS()) {
		// if all goes well, we'll create a token each pass
		Token token;
		token.locBegin = inputStream.GetLocation();

		char c = inputStream.Peek();
		switch (c) {
			case '{':
				token.sValue = c;
				inputStream.Get();
				token.nType = Token::TOKEN_OBJECT_BEGIN;
				break;

			case '}':
				token.sValue = c;
				inputStream.Get();
				token.nType = Token::TOKEN_OBJECT_END;
				break;

			case '[':
				token.sValue = c;
				inputStream.Get();
				token.nType = Token::TOKEN_ARRAY_BEGIN;
				break;

			case ']':
				token.sValue = c;
				inputStream.Get();
				token.nType = Token::TOKEN_ARRAY_END;
				break;

			case ',':
				token.sValue = c;
				inputStream.Get();
				token.nType = Token::TOKEN_NEXT_ELEMENT;
				break;

			case ':':
				token.sValue = c;
				inputStream.Get();
				token.nType = Token::TOKEN_MEMBER_ASSIGN;
				break;

			case '"':
				MatchString(token.sValue, inputStream);
				token.nType = Token::TOKEN_STRING;
				break;

			case '-':
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
				MatchNumber(token.sValue, inputStream);
				token.nType = Token::TOKEN_NUMBER;
				break;

			case 't':
				token.sValue = "true";
				MatchExpectedString(token.sValue, inputStream);
				token.nType = Token::TOKEN_BOOLEAN;
				break;

			case 'f':
				token.sValue = "false";
				MatchExpectedString(token.sValue, inputStream);
				token.nType = Token::TOKEN_BOOLEAN;
				break;

			case 'n':
				token.sValue = "null";
				MatchExpectedString(token.sValue, inputStream);
				token.nType = Token::TOKEN_NULL;
				break;

			case 0:
				return;

			default:
				throw ScanException(std::string("Unexpected character in stream: ") + c, inputStream.GetLocation());
		}

		token.locEnd = inputStream.GetLocation();
		tokens.push_back(token);
	}
}

void Reader::EatWhiteSpace(InputStream& inputStream) {
	while (!inputStream.EOS() && ::isspace(inputStream.Peek()))
		inputStream.Get();
}

void Reader::MatchExpectedString(std::string const& sExpected, InputStream& inputStream) {
	for (char c : sExpected) {
		if (inputStream.EOS() || inputStream.Get() != c)
			throw ScanException("Expected string: " + sExpected, inputStream.GetLocation());
	}
}

void Reader::MatchString(std::string& string, InputStream& inputStream) {
	MatchExpectedString("\"", inputStream);
	auto read_hex_quad = [&]() -> uint16_t {
		uint16_t value = 0;
		for (int index = 0; index < 4; ++index) {
			if (inputStream.EOS())
				throw ScanException(
					"Unexpected end of Unicode escape sequence",
					inputStream.GetLocation());
			auto character = inputStream.Get();
			int digit = character >= '0' && character <= '9'
				? character - '0'
				: character >= 'a' && character <= 'f'
					? character - 'a' + 10
					: character >= 'A' && character <= 'F'
						? character - 'A' + 10
						: -1;
			if (digit < 0)
				throw ScanException(
					"Invalid hexadecimal digit in Unicode escape sequence",
					inputStream.GetLocation());
			value = static_cast<uint16_t>((value << 4) | digit);
		}
		return value;
	};
	auto append_utf8 = [&](uint32_t codepoint) {
		if (codepoint <= 0x7f)
			string.push_back(static_cast<char>(codepoint));
		else if (codepoint <= 0x7ff) {
			string.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
			string.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
		}
		else if (codepoint <= 0xffff) {
			string.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
			string.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
			string.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
		}
		else {
			string.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
			string.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
			string.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
			string.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
		}
	};

	while (!inputStream.EOS() && inputStream.Peek() != '"') {
		char c = inputStream.Get();

		// escape?
		if (c == '\\' && !inputStream.EOS()) { // shouldn't have reached the end yet
			c = inputStream.Get();
			switch (c) {
				case '/':  string.push_back('/');  break;
				case '"':  string.push_back('"');  break;
				case '\\': string.push_back('\\'); break;
				case 'b':  string.push_back('\b'); break;
				case 'f':  string.push_back('\f'); break;
				case 'n':  string.push_back('\n'); break;
				case 'r':  string.push_back('\r'); break;
				case 't':  string.push_back('\t'); break;
				case 'u': {
					auto first = read_hex_quad();
					uint32_t codepoint = first;
					if (first >= 0xd800 && first <= 0xdbff) {
						if (inputStream.EOS() || inputStream.Get() != '\\' ||
							inputStream.EOS() || inputStream.Get() != 'u')
							throw ScanException(
								"High surrogate is not followed by a Unicode low surrogate",
								inputStream.GetLocation());
						auto second = read_hex_quad();
						if (second < 0xdc00 || second > 0xdfff)
							throw ScanException(
								"High surrogate is not followed by a Unicode low surrogate",
								inputStream.GetLocation());
						codepoint = 0x10000 +
							((static_cast<uint32_t>(first) - 0xd800) << 10) +
							(static_cast<uint32_t>(second) - 0xdc00);
					}
					else if (first >= 0xdc00 && first <= 0xdfff) {
						throw ScanException(
							"Unicode low surrogate is missing its high surrogate",
							inputStream.GetLocation());
					}
					append_utf8(codepoint);
					break;
				}
				default:
					throw ScanException(std::string("Unrecognized escape sequence found in string: \\") + c, inputStream.GetLocation());
			}
		}
		else {
			string.push_back(c);
		}
	}

	// eat the last '"' that we hopefully just peeked
	MatchExpectedString("\"", inputStream);
}

void Reader::MatchNumber(std::string& sNumber, InputStream& inputStream) {
	const char numericChars[] = "0123456789.eE-+";
	while (!inputStream.EOS() && std::find(numericChars, std::end(numericChars), inputStream.Peek()) != std::end(numericChars))
		sNumber.push_back(inputStream.Get());
}

UnknownElement Reader::Parse(Reader::TokenStream& tokenStream) {
	if (tokenStream.EOS())
		throw ParseException("Unexpected end of token stream", Location(), Location()); // nowhere to point to

	Token const& token = tokenStream.Peek();
	switch (token.nType) {
		case Token::TOKEN_OBJECT_BEGIN: return ParseObject(tokenStream);
		case Token::TOKEN_ARRAY_BEGIN:  return ParseArray(tokenStream);
		case Token::TOKEN_STRING:       return ParseString(tokenStream);
		case Token::TOKEN_NUMBER:       return ParseNumber(tokenStream);
		case Token::TOKEN_BOOLEAN:      return ParseBoolean(tokenStream);
		case Token::TOKEN_NULL:         return ParseNull(tokenStream);
		default:
			throw ParseException("Unexpected token: " + token.sValue, token.locBegin, token.locEnd);
	}
}

UnknownElement Reader::ParseObject(Reader::TokenStream& tokenStream) {
	MatchExpectedToken(Token::TOKEN_OBJECT_BEGIN, tokenStream);

	Object object;

	while (!tokenStream.EOS() && tokenStream.Peek().nType != Token::TOKEN_OBJECT_END) {
		// first the member name. save the token in case we have to throw an exception
		Token const& tokenName = tokenStream.Peek();
		std::string const& name = MatchExpectedToken(Token::TOKEN_STRING, tokenStream);

		if (object.count(name))
			throw ParseException("Duplicate object member token: " + name, tokenName.locBegin, tokenName.locEnd);

		// ...then the key/value separator...
		MatchExpectedToken(Token::TOKEN_MEMBER_ASSIGN, tokenStream);

		// ...then the value itself (can be anything).
		object[name] = Parse(tokenStream);

		if (!tokenStream.EOS() && tokenStream.Peek().nType != Token::TOKEN_OBJECT_END)
			MatchExpectedToken(Token::TOKEN_NEXT_ELEMENT, tokenStream);
	}

	MatchExpectedToken(Token::TOKEN_OBJECT_END, tokenStream);

	return std::move(object);
}

UnknownElement Reader::ParseArray(Reader::TokenStream& tokenStream) {
	MatchExpectedToken(Token::TOKEN_ARRAY_BEGIN, tokenStream);

	Array array;

	while (!tokenStream.EOS() && tokenStream.Peek().nType != Token::TOKEN_ARRAY_END) {
		array.push_back(Parse(tokenStream));

		if (!tokenStream.EOS() && tokenStream.Peek().nType != Token::TOKEN_ARRAY_END)
			MatchExpectedToken(Token::TOKEN_NEXT_ELEMENT, tokenStream);
	}

	MatchExpectedToken(Token::TOKEN_ARRAY_END, tokenStream);

	return std::move(array);
}

UnknownElement Reader::ParseString(Reader::TokenStream& tokenStream) {
	return MatchExpectedToken(Token::TOKEN_STRING, tokenStream);
}

UnknownElement Reader::ParseNumber(Reader::TokenStream& tokenStream) {
	Token const& currentToken = tokenStream.Peek(); // might need this later for throwing exception
	std::string const& sValue = MatchExpectedToken(Token::TOKEN_NUMBER, tokenStream);

	// First try to parse it as an int
	boost::interprocess::ibufferstream iStr(sValue.data(), sValue.size());
	int64_t iValue;
	iStr >> iValue;

	// If the entire token was consumed then it's not a double
	if (iStr.eof())
		return iValue;

	// Try again as a double
	iStr.seekg(0, std::ios::beg);
	double dValue;
	iStr >> dValue;

	// If there's still stuff left in the token then it's malformed
	if (!iStr.eof())
		throw ParseException(std::string("Unexpected character in NUMBER token: ") + (char)iStr.peek(),
			currentToken.locBegin, currentToken.locEnd);

	return dValue;
}

UnknownElement Reader::ParseBoolean(Reader::TokenStream& tokenStream) {
	return MatchExpectedToken(Token::TOKEN_BOOLEAN, tokenStream) == "true";
}

UnknownElement Reader::ParseNull(Reader::TokenStream& tokenStream) {
	MatchExpectedToken(Token::TOKEN_NULL, tokenStream);
	return Null();
}

std::string const& Reader::MatchExpectedToken(Token::Type nExpected, Reader::TokenStream& tokenStream) {
	if (tokenStream.EOS())
		throw ParseException("Unexpected End of token stream", Location(), Location()); // nowhere to point to

	Token const& token = tokenStream.Get();
	if (token.nType != nExpected)
		throw ParseException("Unexpected token: " + token.sValue, token.locBegin, token.locEnd);

	return token.sValue;
}

}
