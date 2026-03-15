#pragma once

#if defined(_MSC_VER)
#define AGI_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define AGI_NOINLINE __attribute__((noinline))
#else
#define AGI_NOINLINE
#endif

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(likely) && __has_cpp_attribute(unlikely)
#define AGI_UNLIKELY_IF(expr) if (expr) [[unlikely]]
#else
#define AGI_UNLIKELY_IF(expr) if (expr)
#endif
#else
#define AGI_UNLIKELY_IF(expr) if (expr)
#endif
