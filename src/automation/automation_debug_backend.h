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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include "automation_debug_session.h"
#include "automation_invocation.h"

namespace Automation4 {
	class AutomationDebugBackend {
		AutomationDebugSession *session = nullptr;
		bool invocation_active = false;

	protected:
		virtual void OnDebugStateChanged() { }

	public:
		virtual ~AutomationDebugBackend() = default;

		virtual void CaptureRuntimeBaseline() { }

		void SetSession(AutomationDebugSession *next_session)
		{
			session = next_session;
			OnDebugStateChanged();
		}

		AutomationDebugSession *GetSession() const
		{
			return session;
		}

		bool InvocationActive() const
		{
			return invocation_active;
		}

		bool DebugActive() const
		{
			return session && session->Enabled() && invocation_active;
		}

		void BeginInvocation(AutomationInvocation const& invocation)
		{
			if (!session || !session->Enabled())
				return;

			session->BeginInvocation(invocation);
			invocation_active = true;
			OnDebugStateChanged();
		}

		void EndInvocation()
		{
			if (session && invocation_active)
				session->EndInvocation();
			invocation_active = false;
			OnDebugStateChanged();
		}
	};

	class ScopedAutomationDebugInvocation final {
		AutomationDebugBackend *backend = nullptr;

	public:
		ScopedAutomationDebugInvocation(AutomationDebugBackend *backend, AutomationInvocation const& invocation)
		: backend(backend)
		{
			if (this->backend)
				this->backend->BeginInvocation(invocation);
		}

		ScopedAutomationDebugInvocation(ScopedAutomationDebugInvocation const&) = delete;
		ScopedAutomationDebugInvocation& operator=(ScopedAutomationDebugInvocation const&) = delete;
		ScopedAutomationDebugInvocation(ScopedAutomationDebugInvocation&&) = delete;
		ScopedAutomationDebugInvocation& operator=(ScopedAutomationDebugInvocation&&) = delete;

		~ScopedAutomationDebugInvocation()
		{
			if (backend)
				backend->EndInvocation();
		}
	};
}
