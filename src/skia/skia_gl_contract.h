#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

struct SkiaGlContextToken {
	void const *identity = nullptr;
	std::uint64_t generation = 0;
};

enum class SkiaGlDeviceHealth {
	Uninitialized,
	Healthy,
	Unhealthy,
	Abandoned,
};

enum class SkiaGlDeviceFailure {
	None,
	InvalidContextIdentity,
	InvalidContextGeneration,
	ContextActivationFailed,
	ContextIdentityMismatch,
	ContextGenerationMismatch,
	WrongThread,
	ContextInitializationInjected,
	GlVersionUnsupported,
	SoftwareRendererUnsupported,
	GlInterfaceUnavailable,
	GaneshContextUnavailable,
	GaneshContextAbandoned,
	GaneshOutOfMemory,
	InvalidFrameTarget,
	StalePresentGeneration,
	FrameAlreadyOpen,
	FrameNotOpen,
	FrameBeginInjected,
	SurfaceAllocationFailed,
	SurfaceAcquisitionFailed,
	ContentShaderUnavailable,
	ContentUploadFailed,
	FlushInjected,
	SwapBuffersFailed,
	UnsupportedFailureInjection,
};

char const *ToString(SkiaGlDeviceHealth health) noexcept;
char const *ToString(SkiaGlDeviceFailure failure) noexcept;

/// Skia M146's Ganesh desktop interface rejects contexts older than GL 2.0.
/// Keep this check independent of the driver-facing device so unsupported
/// Windows software/remote contexts fail before Skia probes entry points.
bool SupportsSkiaGaneshDesktopGl(std::string_view version) noexcept;

class SkiaGlDeviceState {
	bool owner_bound = false;
	SkiaGlContextToken owner;
	std::thread::id owner_thread;
	SkiaGlDeviceHealth health = SkiaGlDeviceHealth::Uninitialized;
	SkiaGlDeviceFailure last_failure = SkiaGlDeviceFailure::None;
	std::string last_failure_detail;

public:
	bool BeginAccess(SkiaGlContextToken token, std::thread::id current_thread);
	bool MatchesOwner(SkiaGlContextToken token, std::thread::id current_thread) const noexcept;
	void MarkHealthy() noexcept;
	void Fail(SkiaGlDeviceFailure failure, std::string detail);
	void MarkAbandoned() noexcept;

	bool IsOwnerBound() const noexcept { return owner_bound; }
	SkiaGlContextToken Owner() const noexcept { return owner; }
	std::thread::id OwnerThread() const noexcept { return owner_thread; }
	SkiaGlDeviceHealth Health() const noexcept { return health; }
	SkiaGlDeviceFailure LastFailure() const noexcept { return last_failure; }
	std::string const& LastFailureDetail() const noexcept { return last_failure_detail; }
};
