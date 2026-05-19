#pragma once

#if CONFLUX_HAS_WARNING_CLEAN_AUTO_UNDERSCORE_DISCARD
	#define CONFLUX_DISCARD(expr) auto _ = expr
#else
	#define CONFLUX_DISCARD(expr) \
		do { \
			static_cast<void>(expr); /* NOLINT(bugprone-unused-return-value) */ \
		} while (false)
#endif
