#if !defined(HIKARI_LOG_H)
#define HIKARI_LOG_H

#include <stdio.h>

enum hikari_log_level {
  HIKARI_LOG_ERROR = 0,
  HIKARI_LOG_WARN  = 1,
  HIKARI_LOG_INFO  = 2,
  HIKARI_LOG_DEBUG = 3,
  HIKARI_LOG_TRACE = 4,
};

extern enum hikari_log_level hikari_log_level;
extern const char *hikari_log_level_names[];

void hikari_log_init(enum hikari_log_level level);

#define hikari_log(level, fmt, ...)                                    \
  do {                                                                 \
    if ((level) <= hikari_log_level) {                                 \
      fprintf(stderr, "[HIKARI %s] %s:%s: " fmt "\n",                 \
          hikari_log_level_names[(level)],                             \
          __FILE__, __func__, ##__VA_ARGS__);                          \
    }                                                                  \
  } while (0)

#define hikari_log_error(fmt, ...) hikari_log(HIKARI_LOG_ERROR, fmt, ##__VA_ARGS__)
#define hikari_log_warn(fmt, ...)  hikari_log(HIKARI_LOG_WARN, fmt, ##__VA_ARGS__)
#define hikari_log_info(fmt, ...)  hikari_log(HIKARI_LOG_INFO, fmt, ##__VA_ARGS__)

#ifdef NDEBUG
  #define hikari_log_debug(fmt, ...) ((void)0)
  #define hikari_log_trace(fmt, ...) ((void)0)
#else
  #define hikari_log_debug(fmt, ...) hikari_log(HIKARI_LOG_DEBUG, fmt, ##__VA_ARGS__)
  #define hikari_log_trace(fmt, ...) hikari_log(HIKARI_LOG_TRACE, fmt, ##__VA_ARGS__)
#endif

#endif
