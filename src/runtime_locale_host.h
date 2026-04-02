#pragma once

#include <functional>
#include <string>
#include <vector>

struct RuntimeLocaleHost {
	std::function<std::vector<std::string>()> get_available_languages;
	std::function<std::string(std::vector<std::string> const&)> find_preferred_language;
	std::function<void(std::string const&)> initialize_language;
	std::function<bool(std::string const&)> has_language;
};
