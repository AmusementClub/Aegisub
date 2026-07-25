// One-shot: GDI horizontal vs vertical ('@') faces from one EnumFontFamiliesExW.
#include "gdi_font_resolver.h"

#include <libaegisub/charset_conv_win.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

std::optional<std::wstring> ToUtf16(std::string const& value) {
	try {
		return agi::charset::ConvertW(value);
	}
	catch (...) {
		return std::nullopt;
	}
}

bool OrdinalIcaseEqual(std::string const& left, std::string const& right) {
	auto const lw = ToUtf16(left);
	auto const rw = ToUtf16(right);
	return lw && rw &&
		CompareStringOrdinal(
			lw->data(), static_cast<int>(lw->size()),
			rw->data(), static_cast<int>(rw->size()), TRUE) == CSTR_EQUAL;
}

} // namespace

int main() {
	GdiFontResolver resolver;
	if (!resolver.available()) {
		std::cerr << "gdi unavailable\n";
		return 1;
	}

	auto const faces = resolver.EnumerateAllFamilies();
	std::cout << "EnumFontFamiliesExW single pass (EnumerateAllFamilies)\n"
	          << "  horizontal_faces=" << faces.horizontal.size() << '\n'
	          << "  vertical_@faces=" << faces.vertical.size() << '\n';

	int paired = 0;
	int orphan = 0;
	std::cout << "\nvertical without ordinal-icase horizontal pair:\n";
	for (auto const& v : faces.vertical) {
		if (v.empty() || v.front() != '@')
			continue;
		auto const bare = v.substr(1);
		bool found = false;
		for (auto const& h : faces.horizontal) {
			if (OrdinalIcaseEqual(h, bare)) {
				found = true;
				break;
			}
		}
		if (found)
			++paired;
		else {
			++orphan;
			if (orphan <= 30)
				std::cout << "  " << v << '\n';
		}
	}
	std::cout << "\npaired=" << paired << " orphan_vertical=" << orphan << '\n';
	std::cout << "\nsample vertical (first 25):\n";
	for (std::size_t i = 0; i < faces.vertical.size() && i < 25; ++i)
		std::cout << "  " << faces.vertical[i] << '\n';
	return 0;
}
