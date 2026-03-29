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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include "headless_runtime_bootstrap.h"

#include <vector>
#include <string>

#include <wx/app.h>
#include <wx/init.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

namespace {

std::string WideToUtf8(wchar_t const* value) {
	if (!value)
		return {};
	int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
	if (size <= 1)
		return {};
	std::string out(static_cast<size_t>(size - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), size, nullptr, nullptr);
	return out;
}

std::vector<std::string> CurrentProcessArgs() {
	int argc = 0;
	auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv)
		return {};

	struct ScopedArgv final {
		wchar_t** argv = nullptr;
		~ScopedArgv() {
			if (argv)
				LocalFree(argv);
		}
	} scoped{argv};

	std::vector<std::string> args;
	args.reserve(argc);
	for (int i = 0; i < argc; ++i)
		args.push_back(WideToUtf8(argv[i]));
	return args;
}

}

extern "C" int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, wxCmdLineArgType lpCmdLine, int nCmdShow) {
	wxDISABLE_DEBUG_SUPPORT();

	auto const args = CurrentProcessArgs();
	if (IsHeadlessCommandLine(args)) {
		wxInitializer wx_initializer;
		if (!wx_initializer.IsOk())
			return 2;
		auto const exit_code = RunHeadlessCommandLine(args);
		return exit_code;
	}

	return wxEntry(hInstance, hPrevInstance, lpCmdLine, nCmdShow);
}

#else

int main(int argc, char** argv) {
	wxDISABLE_DEBUG_SUPPORT();

	std::vector<std::string> args;
	args.reserve(argc);
	for (int i = 0; i < argc; ++i)
		args.emplace_back(argv[i]);

	if (IsHeadlessCommandLine(args))
		return RunHeadlessCommandLine(args);

	return wxEntry(argc, argv);
}

#endif
