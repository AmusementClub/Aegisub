#pragma once

#include <string>
#include <vector>

bool IsHeadlessCommandLine(std::vector<std::string> const& args);
int RunHeadlessCommandLine(std::vector<std::string> const& args);
