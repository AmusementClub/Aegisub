#pragma once

#include "app_runtime.h"

#include <memory>
#include <string>

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
