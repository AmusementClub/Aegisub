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

#include <libaegisub/string_utils.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {
using agi::util::strings::sized_match;
using agi::util::strings::utf8_find_icase;
using agi::util::strings::utf8_icase_searcher;

using clock_type = std::chrono::steady_clock;

struct Scenario {
	std::string name;
	std::vector<std::string> haystacks;
	std::string needle;
};

std::vector<std::string> make_indexed_lines(std::string_view prefix, std::string_view suffix, std::size_t count) {
	std::vector<std::string> out;
	out.reserve(count);
	for (std::size_t i = 0; i < count; ++i) {
		std::string line(prefix);
		line += std::to_string(i & 255);
		line.append(suffix.data(), suffix.size());
		out.emplace_back(std::move(line));
	}
	return out;
}

template<typename Func>
double measure_ns_per_search(Func&& func, std::size_t searches_per_iteration, std::size_t iterations) {
	volatile std::size_t sink = 0;
	auto begin = clock_type::now();
	for (std::size_t i = 0; i < iterations; ++i)
		sink += func();
	auto elapsed = std::chrono::duration<double, std::nano>(clock_type::now() - begin).count();
	if (sink == std::numeric_limits<std::size_t>::max())
		std::cerr << "unreachable sink: " << sink << '\n';
	return elapsed / static_cast<double>(searches_per_iteration * iterations);
}

bool matches_equal(sized_match const& left, sized_match const& right) {
	return left.offset == right.offset && left.length == right.length;
}

template<typename Func>
std::size_t run_searches(std::vector<std::string> const& haystacks, Func&& search) {
	std::size_t sum = 0;
	for (auto const& haystack : haystacks) {
		auto match = search(haystack);
		sum += match ? match.offset + match.length + 1 : 0;
	}
	return sum;
}

std::vector<Scenario> make_scenarios() {
	return {
		{
			"ascii-hit",
			make_indexed_lines("Alpha beta gamma delta line ", " UniqueNeedle", 4096),
			"uniqueneedle"
		},
		{
			"ascii-miss",
			make_indexed_lines("Alpha beta gamma delta line ", " with no match", 4096),
			"needle-not-present"
		},
		{
			"latin-hit",
			make_indexed_lines("caf\xC3\xA9 d\xC3\xA9j\xC3\xA0 vu line ", " Stra\xC3\x9F""e", 4096),
			"STRASSE"
		},
		{
			"latin-miss",
			make_indexed_lines("caf\xC3\xA9 d\xC3\xA9j\xC3\xA0 vu line ", " sans match", 4096),
			"STRASSE"
		},
		{
			"cjk-hit",
			make_indexed_lines("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C line ", " \xE5\xAD\x97\xE5\xB9\x95\xE6\x90\x9C\xE7\xB4\xA2", 4096),
			"\xE5\xAD\x97\xE5\xB9\x95\xE6\x90\x9C\xE7\xB4\xA2"
		},
		{
			"cjk-miss",
			make_indexed_lines("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C line ", " \xE6\xB2\xA1\xE6\x9C\x89\xE7\x9B\xAE\xE6\xA0\x87", 4096),
			"\xE5\xAD\x97\xE5\xB9\x95\xE6\x90\x9C\xE7\xB4\xA2"
		},
		{
			"mixed-hit",
			make_indexed_lines("Cafe \xE4\xBD\xA0\xE5\xA5\xBD line ", " Stra\xC3\x9F""e \xCE\x91\xCE\x92\xCE\x93", 4096),
			"strasse \xCE\xB1\xCE\xB2\xCE\xB3"
		},
		{
			"mixed-miss",
			make_indexed_lines("Cafe \xE4\xBD\xA0\xE5\xA5\xBD line ", " Stra\xC3\x9F""e \xCE\x91\xCE\x92\xCE\x93", 4096),
			"strasse \xE6\xB2\xA1\xE6\x9C\x89"
		}
	};
}
}

int main() {
#ifdef AEGISUB_USE_STRINGZILLA
	std::cout << "search-replace bench (StringZilla enabled)\n\n";
#else
	std::cout << "search-replace bench (StringZilla disabled)\n\n";
#endif

	const auto scenarios = make_scenarios();
	const std::size_t iterations = 300;
	double total_fresh = 0.0;
	double total_cached = 0.0;

	std::cout << std::left
	          << std::setw(14) << "scenario"
	          << std::right
	          << std::setw(18) << "fresh ns/search"
	          << std::setw(20) << "cached ns/search"
	          << std::setw(12) << "speedup"
	          << '\n';

	for (auto const& scenario : scenarios) {
		auto searcher = utf8_icase_searcher(scenario.needle);
		for (auto const& haystack : scenario.haystacks) {
			auto direct_match = utf8_find_icase(haystack, scenario.needle);
			auto cached_match = utf8_find_icase(haystack, searcher);
			if (!matches_equal(direct_match, cached_match)) {
				std::cerr << "match mismatch for " << scenario.name << '\n';
				return 1;
			}
		}

		(void)measure_ns_per_search([&] {
			return run_searches(scenario.haystacks, [&](std::string const& haystack) {
				return utf8_find_icase(haystack, scenario.needle);
			});
		}, scenario.haystacks.size(), 10);
		(void)measure_ns_per_search([&] {
			return run_searches(scenario.haystacks, [&](std::string const& haystack) {
				return utf8_find_icase(haystack, searcher);
			});
		}, scenario.haystacks.size(), 10);

		auto fresh = measure_ns_per_search([&] {
			return run_searches(scenario.haystacks, [&](std::string const& haystack) {
				return utf8_find_icase(haystack, scenario.needle);
			});
		}, scenario.haystacks.size(), iterations);
		auto cached = measure_ns_per_search([&] {
			return run_searches(scenario.haystacks, [&](std::string const& haystack) {
				return utf8_find_icase(haystack, searcher);
			});
		}, scenario.haystacks.size(), iterations);

		total_fresh += fresh;
		total_cached += cached;

		std::cout << std::left
		          << std::setw(14) << scenario.name
		          << std::right
		          << std::setw(18) << std::fixed << std::setprecision(1) << fresh
		          << std::setw(20) << std::fixed << std::setprecision(1) << cached
		          << std::setw(12) << std::fixed << std::setprecision(2) << (fresh / cached)
		          << '\n';
	}

	std::cout << "\n";
	std::cout << std::left
	          << std::setw(14) << "overall"
	          << std::right
	          << std::setw(18) << std::fixed << std::setprecision(1) << (total_fresh / scenarios.size())
	          << std::setw(20) << std::fixed << std::setprecision(1) << (total_cached / scenarios.size())
	          << std::setw(12) << std::fixed << std::setprecision(2) << (total_fresh / total_cached)
	          << '\n';

	return 0;
}
