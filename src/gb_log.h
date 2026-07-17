#pragma once
#include <cstdarg>

enum LogLevel {
    LOG_TRACE = 0,
    LOG_DEBUG = 1,
    LOG_INFO  = 2,
    LOG_WARN  = 3,
    LOG_ERROR = 4,
};

enum LogCat {
    LOG_SYS = 0,
    LOG_CPU,
    LOG_MEM,
    LOG_TIMER,
    LOG_PPU,
    LOG_APU,
    LOG_CART,
    LOG_SERIAL,
    LOG_CAT_COUNT,
};

typedef void (*LogSink)(LogLevel level, LogCat cat, const char* text);

void log_set_sink(LogSink sink);

void log_set_min_level(LogLevel level);
LogLevel log_get_min_level();

const char* log_level_name(LogLevel level);
const char* log_cat_name(LogCat cat);

void log_msg(LogLevel level, LogCat cat, const char* fmt, ...);
void log_msg_v(LogLevel level, LogCat cat, const char* fmt, va_list args);

#define LOGT(cat, ...) log_msg(LOG_TRACE, cat, __VA_ARGS__)
#define LOGD(cat, ...) log_msg(LOG_DEBUG, cat, __VA_ARGS__)
#define LOGI(cat, ...) log_msg(LOG_INFO,  cat, __VA_ARGS__)
#define LOGW(cat, ...) log_msg(LOG_WARN,  cat, __VA_ARGS__)
#define LOGE(cat, ...) log_msg(LOG_ERROR, cat, __VA_ARGS__)

void log_print_banner(const char* subtitle);
