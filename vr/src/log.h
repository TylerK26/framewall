// Timestamped logging to stderr (the launcher sends it to a log file).
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <sys/time.h>

__attribute__((format(printf, 1, 2))) inline void fwlog(const char *fmt, ...) {
    timeval tv;
    gettimeofday(&tv, nullptr);
    tm t;
    localtime_r(&tv.tv_sec, &t);
    char ts[32];
    std::strftime(ts, sizeof ts, "%H:%M:%S", &t);
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "%s.%03ld %s\n", ts, long(tv.tv_usec / 1000), msg);
}
