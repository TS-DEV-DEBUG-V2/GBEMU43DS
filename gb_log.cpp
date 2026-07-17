#include "gb_log.h"
#include <cstdio>
#include <cstring>

static LogSink g_sink = nullptr;
static LogLevel g_min_level = LOG_INFO;

void log_set_sink(LogSink sink) { g_sink = sink; }
void log_set_min_level(LogLevel level) { g_min_level = level; }
LogLevel log_get_min_level() { return g_min_level; }

const char* log_level_name(LogLevel level) {
    switch (level) {
        case LOG_TRACE: return "TRACE";
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
    }
    return "?";
}

const char* log_cat_name(LogCat cat) {
    switch (cat) {
        case LOG_SYS:    return "SYS";
        case LOG_CPU:    return "CPU";
        case LOG_MEM:    return "MEM";
        case LOG_TIMER:  return "TIMER";
        case LOG_PPU:    return "PPU";
        case LOG_APU:    return "APU";
        case LOG_CART:   return "CART";
        case LOG_SERIAL: return "SERIAL";
        default:         return "?";
    }
}

void log_msg_v(LogLevel level, LogCat cat, const char* fmt, va_list args) {
    if (level < g_min_level || !g_sink) return;
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    g_sink(level, cat, buf);
}

void log_msg(LogLevel level, LogCat cat, const char* fmt, ...) {
    if (level < g_min_level || !g_sink) return;
    va_list args;
    va_start(args, fmt);
    log_msg_v(level, cat, fmt, args);
    va_end(args);
}

static const char* BANNER_ART = R"(
    mmmm   mmmmmm    mmmmmmmm  mmm  mmm  mm    mm
  ##""""#  ##""""##  ##""""""  ###  ###  ##    ##
 ##        ##    ##  ##        ########  ##    ##
 ##  mmmm  #######   #######   ## ## ##  ##    ##
 ##  ""##  ##    ##  ##        ## "" ##  ##    ##
  ##mmm##  ##mmmm##  ##mmmmmm  ##    ##  "##mm##"
    """"   """""""   """"""""  ""    ""    """"
writen by TS-DEV-DEBUG-V2 on Github
Copyright 2026
LICENSE: GPL-3.0
enjoy yo game.
)";

void log_print_banner(const char* subtitle) {
    if (!g_sink) return;
    const char* p = BANNER_ART;
    char line[128];
    while (*p) {
        int n = 0;
        while (*p && *p != '\n' && n < (int)sizeof(line) - 1) line[n++] = *p++;
        line[n] = '\0';
        if (*p == '\n') p++;
        g_sink(LOG_INFO, LOG_SYS, line);
    }
    g_sink(LOG_INFO, LOG_SYS, "Emulation has started!");
    if (subtitle && subtitle[0]) {
        g_sink(LOG_INFO, LOG_SYS, subtitle);
    }
}
