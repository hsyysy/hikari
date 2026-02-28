#include <hikari/log.h>

#include <wlr/util/log.h>

#ifdef NDEBUG
enum hikari_log_level hikari_log_level = HIKARI_LOG_INFO;
#else
enum hikari_log_level hikari_log_level = HIKARI_LOG_DEBUG;
#endif

const char *hikari_log_level_names[] = {
  [HIKARI_LOG_ERROR] = "ERROR",
  [HIKARI_LOG_WARN]  = "WARN",
  [HIKARI_LOG_INFO]  = "INFO",
  [HIKARI_LOG_DEBUG] = "DEBUG",
  [HIKARI_LOG_TRACE] = "TRACE",
};

void
hikari_log_init(enum hikari_log_level level)
{
  hikari_log_level = level;

  enum wlr_log_importance wlr_level;
  if (level <= HIKARI_LOG_ERROR) {
    wlr_level = WLR_ERROR;
  } else if (level <= HIKARI_LOG_INFO) {
    wlr_level = WLR_INFO;
  } else {
    wlr_level = WLR_DEBUG;
  }

  wlr_log_init(wlr_level, NULL);
}
