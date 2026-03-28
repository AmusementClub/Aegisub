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

#include "async_video_provider_host.h"

#include <utility>

AsyncVideoProviderEventSink CreateAsyncVideoProviderWxEventSink(
	wxEvtHandler *parent,
	agi::ui::WeakLifetime event_lifetime) {
	return [parent, event_lifetime](std::unique_ptr<wxEvent> evt) mutable {
		if (!parent)
			return;
		agi::ui::MainAsyncIfAlive(event_lifetime, [parent, evt = std::move(evt)]() mutable {
			parent->QueueEvent(evt.release());
		});
	};
}
