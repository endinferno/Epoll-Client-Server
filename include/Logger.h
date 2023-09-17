#pragma once

#define INFO_ENABLE 1
#define DEBUG_ENABLE 1
#define ERROR_ENABLE 1

#if INFO_ENABLE
#define INFO(fmt, ...)                            \
	do {                                          \
		fprintf(stdout, fmt "\n", ##__VA_ARGS__); \
	} while (0)
#else
#define INFO(fmt, ...) \
	do {               \
	} while (0);
#endif

#if DEBUG_ENABLE
#define DEBUG(fmt, ...)                                                                      \
	do {                                                                                     \
		fprintf(stdout, "FUNC: %s, LINE: %d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
	} while (0)
#else
#define DEBUG(fmt, ...) \
	do {                \
	} while (0);
#endif

#if ERROR_ENABLE
#define ERROR(fmt, ...)                                                                      \
	do {                                                                                     \
		fprintf(stderr, "FUNC: %s, LINE: %d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
	} while (0)
#else
#define ERROR(fmt, ...) \
	do {                \
	} while (0);
#endif
