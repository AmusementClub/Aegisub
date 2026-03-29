#pragma once

#include "app_runtime.h"

#include <memory>
#include <string>

// This host isolates the minimal wx runtime required by current headless
// bring-up. Replace or remove it when headless runtime no longer depends on wx.
class HeadlessWxRuntimeHost {
	struct Impl;
	std::unique_ptr<Impl> impl;

public:
	HeadlessWxRuntimeHost();
	~HeadlessWxRuntimeHost();

	HeadlessWxRuntimeHost(HeadlessWxRuntimeHost const&) = delete;
	HeadlessWxRuntimeHost& operator=(HeadlessWxRuntimeHost const&) = delete;
	HeadlessWxRuntimeHost(HeadlessWxRuntimeHost&&) = delete;
	HeadlessWxRuntimeHost& operator=(HeadlessWxRuntimeHost&&) = delete;

	bool Initialize(std::string& error);
	AppRuntimeHostHooks BuildRuntimeHooks() const;
};
