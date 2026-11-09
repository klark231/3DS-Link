#pragma once
// Logging goes to logcat and to the bridge's own log file (see hook.cpp).
#include <string>
#include <fmt/format.h>
namespace MmjHub {
void Log(int level, const std::string& text);
}
#define MMJ_LOG(level, cls, ...) ::MmjHub::Log(level, fmt::format(__VA_ARGS__))
#define LOG_TRACE(cls, ...) do {} while (0)
#define LOG_DEBUG(cls, ...) MMJ_LOG(3, cls, __VA_ARGS__)
#define LOG_INFO(cls, ...) MMJ_LOG(4, cls, __VA_ARGS__)
#define LOG_WARNING(cls, ...) MMJ_LOG(5, cls, __VA_ARGS__)
#define LOG_ERROR(cls, ...) MMJ_LOG(6, cls, __VA_ARGS__)
#define LOG_CRITICAL(cls, ...) MMJ_LOG(6, cls, __VA_ARGS__)
