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
	FlushInjected,
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

enum class SkiaVideoFailureInjection {
	None,
	ContextInitialization,
	FrameBegin,
	FlushSubmit,
	Unsupported,
};

SkiaVideoFailureInjection ParseSkiaVideoFailureInjection(std::string_view value) noexcept;
char const *ToString(SkiaVideoFailureInjection injection) noexcept;

enum class SkiaVideoTargetOrigin {
	Unknown,
	BottomLeft,
	TopLeft,
};

enum class SkiaVideoTargetPixelFormat {
	Unknown,
	Rgba8,
};

enum class SkiaVideoTargetColorSpace {
	Unknown,
	SdrPreview,
};

struct SkiaVideoTargetViewport {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

struct SkiaVideoFrameTarget {
	unsigned int framebuffer_id = 0;
	std::uint64_t context_generation = 0;
	int width = 0;
	int height = 0;
	SkiaVideoTargetViewport viewport;
	SkiaVideoTargetOrigin origin = SkiaVideoTargetOrigin::Unknown;
	int sample_count = 0;
	int stencil_bits = 0;
	SkiaVideoTargetPixelFormat pixel_format = SkiaVideoTargetPixelFormat::Unknown;
	SkiaVideoTargetColorSpace color_space = SkiaVideoTargetColorSpace::Unknown;
	bool hdr_to_sdr_complete = false;
	std::uint64_t present_generation = 0;
};

struct SkiaVideoFrameTargetValidation {
	bool valid = false;
	std::string detail;
};

SkiaVideoFrameTargetValidation ValidateSkiaVideoFrameTarget(
	SkiaVideoFrameTarget const& target,
	SkiaGlContextToken context);
