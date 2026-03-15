// Copyright (c) 2015, Thomas Goyne <plorkyeran@aegisub.org>
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
//
// Aegisub Project http://www.aegisub.org/

#include <iterator>
#include <string>
#include <string_view>

namespace agi {
	template<typename Iterator>
	class iterator_range {
		Iterator first;
		Iterator last;
	public:
		using iterator = Iterator;
		using const_iterator = Iterator;

		iterator_range() = default;
		iterator_range(Iterator first, Iterator last) : first(first), last(last) { }

		Iterator begin() const { return first; }
		Iterator end() const { return last; }

		bool empty() const { return first == last; }
		auto size() const { return static_cast<std::size_t>(std::distance(first, last)); }
		auto operator[](std::size_t idx) const -> decltype(*(first + idx)) { return *(first + idx); }
	};

	template<typename Iterator>
	bool operator==(iterator_range<Iterator> const& left, std::string_view right) {
		return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
	}

	template<typename Iterator>
	bool operator==(std::string_view left, iterator_range<Iterator> const& right) {
		return right == left;
	}

	typedef iterator_range<std::string::const_iterator> StringRange;

	template<typename Iterator>
	class split_iterator {
		bool is_end = false;
		Iterator b;
		Iterator cur;
		Iterator e;
		typename Iterator::value_type c;

	public:
		using iterator_category = std::forward_iterator_tag;
		using value_type = iterator_range<Iterator>;
		using pointer = value_type*;
		using reference = value_type&;
		using difference_type = ptrdiff_t;

		split_iterator(Iterator begin, Iterator end, typename Iterator::value_type c)
		: b(begin), cur(begin), e(end), c(c)
		{
			if (b != e)
				cur = std::find(b, e, c);
			else
				is_end = true;
		}

		split_iterator() : is_end(true) { }

		bool eof() const { return is_end; }

		iterator_range<Iterator> operator*() const {
			return iterator_range<Iterator>(b, cur);
		}

		bool operator==(split_iterator const& it) const {
			if (is_end || it.is_end)
				return is_end && it.is_end;
			return b == it.b && cur == it.cur && e == it.e && c == it.c;
		}

		bool operator!=(split_iterator const& it) const {
			return !(*this == it);
		}

		split_iterator& operator++() {
			if (cur != e) {
				b = cur + 1;
				cur = std::find(b, e, c);
			}
			else {
				b = e;
				is_end = true;
			}

			return *this;
		}

		split_iterator operator++(int) {
			split_iterator tmp = *this;
			++*this;
			return tmp;
		}
	};

	template<typename Iterator>
	split_iterator<Iterator> begin(split_iterator<Iterator> const& it) {
		return it;
	}

	template<typename Iterator>
	split_iterator<Iterator> end(split_iterator<Iterator> const&) {
		return split_iterator<Iterator>();
	}

	static inline std::string str(StringRange const& r) {
		return std::string(r.begin(), r.end());
	}

	template<typename Str, typename Char>
	split_iterator<typename Str::const_iterator> Split(Str const& str, Char delim) {
		return split_iterator<typename Str::const_iterator>(begin(str), end(str), delim);
	}

	template<typename Cont, typename Str, typename Char>
	void Split(Cont& out, Str const& str, Char delim) {
		out.clear();
		for (auto const& tok : Split(str, delim))
			out.emplace_back(begin(tok), end(tok));
	}
}
