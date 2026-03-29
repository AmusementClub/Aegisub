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

#include "headless_wx_runtime_host.h"

// Headless wx bring-up is intentionally isolated to this host shell so the
// shared headless bootstrap path can stay wx-free.
#include <wx/image.h>
#include <wx/init.h>
#include <wx/log.h>

struct HeadlessWxRuntimeHost::Impl {
	wxInitializer wx_initializer;
};

HeadlessWxRuntimeHost::HeadlessWxRuntimeHost()
: impl(std::make_unique<Impl>()) {
}

HeadlessWxRuntimeHost::~HeadlessWxRuntimeHost() = default;

bool HeadlessWxRuntimeHost::Initialize(std::string& error) {
	if (impl->wx_initializer.IsOk())
		return true;

	error = "failed to initialize wx runtime for headless mode";
	return false;
}

AppRuntimeHostHooks HeadlessWxRuntimeHost::BuildRuntimeHooks() const {
	return {
		[] {
			(void)wxLog::GetActiveTarget();
		},
		[] {
			wxImage::AddHandler(new wxPNGHandler);
		}
	};
}
