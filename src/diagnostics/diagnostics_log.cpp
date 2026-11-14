#include "diagnostics/diagnostics_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <share.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace forevertas::diagnostics {
namespace {

constexpr int kLinesPerSecond = 100;
constexpr int kBurstLines = 400;

std::atomic<std::FILE *> file{nullptr};
std::mutex mutex;
std::string path;

// Flood control (under mutex).
std::string lastLine;
unsigned long long lastLineRepeats = 0u;
double tokens = kBurstLines;
std::chrono::steady_clock::time_point lastRefill = std::chrono::steady_clock::now();
unsigned long long droppedLines = 0u;

void WriteStamped(std::FILE *target, const char *thread, const char *text) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const long long milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
            1000;
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    std::fprintf(target, "%02d:%02d:%02d.%03lld [%s] %s\n", local.tm_hour, local.tm_min,
                 local.tm_sec, milliseconds, thread, text);
}

}  // namespace

bool OpenLog(const std::string &where) {
    std::FILE *opened = nullptr;
#if defined(_WIN32)
    const int size = MultiByteToWideChar(CP_UTF8, 0, where.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size > 0 ? size : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, where.c_str(), -1, wide.data(), size);
    opened = _wfsopen(wide.c_str(), L"w", _SH_DENYNO);
#else
    opened = std::fopen(where.c_str(), "w");
#endif
    if (opened == nullptr) return false;
    path = where;
    file.store(opened, std::memory_order_release);
    return true;
}

bool LogOpen() noexcept {
    return file.load(std::memory_order_relaxed) != nullptr;
}

const std::string &LogPath() noexcept {
    return path;
}

void Log(const char *format, ...) noexcept {
    std::FILE *target = file.load(std::memory_order_acquire);
    if (target == nullptr) return;
    char line[8192];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    std::ostringstream threadId;
    threadId << std::this_thread::get_id();
    const std::string thread = threadId.str();

    const std::lock_guard<std::mutex> lock(mutex);
    if (lastLine == line) {
        ++lastLineRepeats;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    tokens = std::min<double>(
            kBurstLines,
            tokens + std::chrono::duration<double>(now - lastRefill).count() * kLinesPerSecond);
    lastRefill = now;
    if (tokens < 1.0) {
        ++droppedLines;
        return;
    }
    tokens -= 1.0;
    char note[160];
    if (lastLineRepeats > 0u) {
        std::snprintf(note, sizeof note, "(the line above %llu more times)", lastLineRepeats);
        WriteStamped(target, thread.c_str(), note);
        lastLineRepeats = 0u;
    }
    if (droppedLines > 0u) {
        std::snprintf(note, sizeof note, "(%llu lines left out: too many at once)", droppedLines);
        WriteStamped(target, thread.c_str(), note);
        droppedLines = 0u;
    }
    lastLine = line;
    WriteStamped(target, thread.c_str(), line);
    std::fflush(target);
}

void LogRaw(const char *text) noexcept {
    std::FILE *target = file.load(std::memory_order_acquire);
    if (target == nullptr) return;
    std::fputs(text, target);
    std::fflush(target);
}

int LogDescriptor() noexcept {
    std::FILE *target = file.load(std::memory_order_acquire);
    if (target == nullptr) return -1;
#if defined(_WIN32)
    return _fileno(target);
#else
    return fileno(target);
#endif
}

void LogCleanExit() noexcept {
    std::FILE *target = file.load(std::memory_order_acquire);
    if (target == nullptr) return;
    const std::lock_guard<std::mutex> lock(mutex);
    if (lastLineRepeats > 0u) {
        std::fprintf(target, "(the line above %llu more times)\n", lastLineRepeats);
        lastLineRepeats = 0u;
    }
    std::fprintf(target, "%s\n", kCleanExitLine);
    std::fflush(target);
}

}  // namespace forevertas::diagnostics
