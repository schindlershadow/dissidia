#pragma once

#include "ppsspp_config.h"
#include <cstdint>

#if PPSSPP_PLATFORM(WINDOWS) && defined(_MSC_VER)   // mingw/clang use the builtins below
#include "Common/CommonWindows.h"

// Use this if you know the value is non-zero.
inline uint32_t clz32_nonzero(uint32_t value) {
	DWORD index;
	BitScanReverse(&index, value);
	return 31 ^ (uint32_t)index;
}

inline uint32_t clz32(uint32_t value) {
	if (!value)
		return 32;
	DWORD index;
	BitScanReverse(&index, value);
	return 31 ^ (uint32_t)index;
}

#else

// Use this if you know the value is non-zero.
inline uint32_t clz32_nonzero(uint32_t value) {
	return __builtin_clz(value);
}

inline uint32_t clz32(uint32_t value) {
	if (!value)
		return 32;
	return __builtin_clz(value);
}

#endif
