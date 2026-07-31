#pragma once

#include "skia/skia_gl_contract.h"

#include <memory>
#include <string>

class GrDirectContext;

enum class SkiaGlFailureInjection {
	None,
	ContextInitialization,
	FlushSubmit,
};

class SkiaGlDevice final {
	struct Impl;
	std::unique_ptr<Impl> impl;

public:
	explicit SkiaGlDevice(SkiaGlFailureInjection failure_injection);
	~SkiaGlDevice();

	SkiaGlDevice(SkiaGlDevice const&) = delete;
	SkiaGlDevice& operator=(SkiaGlDevice const&) = delete;

	bool BeginExternalFrame(SkiaGlContextToken token);
	bool FlushAndSubmit(SkiaGlContextToken token);
	void SetFailureInjection(SkiaGlFailureInjection failure_injection) noexcept;
	void ResetTextureBindingsForExternalUse(SkiaGlContextToken token) noexcept;
	void Fail(SkiaGlContextToken token, SkiaGlDeviceFailure failure, std::string detail) noexcept;
	void ReleaseResourcesAndAbandon(SkiaGlContextToken token) noexcept;
	void Abandon() noexcept;

	GrDirectContext *Get() const noexcept;
	SkiaGlDeviceHealth Health() const noexcept;
	SkiaGlDeviceFailure LastFailure() const noexcept;
	std::string const& LastFailureDetail() const noexcept;
	std::string const& GlVendor() const noexcept;
	std::string const& GlRenderer() const noexcept;
	std::string const& GlVersion() const noexcept;
};
