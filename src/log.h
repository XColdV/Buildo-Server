#pragma once
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

inline std::mutex& LogMutex() {
    static std::mutex m;
    return m;
}

inline void LogV(const char* tag, const char* fmt, va_list ap) {
    std::lock_guard<std::mutex> lock(LogMutex());
    time_t now = time(nullptr);
    tm t{};
    localtime_s(&t, &now);
    printf("%02d:%02d:%02d %s", t.tm_hour, t.tm_min, t.tm_sec, tag);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
}

inline void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV("", fmt, ap);
    va_end(ap);
}

inline void LogError(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV("ERROR ", fmt, ap);
    va_end(ap);
}
