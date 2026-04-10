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

#include <memory>
#include <string>

namespace Automation4 {
	class AutomationDebugService;

	class AutomationDebugAdapterConnection {
	public:
		virtual ~AutomationDebugAdapterConnection() = default;

		virtual bool ReadProtocolMessage(std::string& payload, std::string& error) = 0;
		virtual bool WriteProtocolMessage(std::string const& payload, std::string& error) = 0;
		virtual void Close() = 0;
	};

	class AutomationDebugAdapter final {
		class Impl;
		std::unique_ptr<Impl> impl;

	public:
		AutomationDebugAdapter(
			AutomationDebugService& service,
			std::string expected_token,
			std::unique_ptr<AutomationDebugAdapterConnection> connection);
		~AutomationDebugAdapter();

		AutomationDebugAdapter(AutomationDebugAdapter const&) = delete;
		AutomationDebugAdapter& operator=(AutomationDebugAdapter const&) = delete;

		void Stop();
		void Run();
	};
}
