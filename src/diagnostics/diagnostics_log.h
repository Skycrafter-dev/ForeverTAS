#ifndef FOREVERTAS_DIAGNOSTICS_DIAGNOSTICS_LOG_H
#define FOREVERTAS_DIAGNOSTICS_DIAGNOSTICS_LOG_H

#include <string>

// ForeverTAS's log: one text file per session that users can send when
// something goes wrong. Every line is flushed, so the last one before a crash
// is in the file. Lines are for events (a search starts, a map loads, an
// error), never per tick or per frame; a flood (over 100 lines a second, or
// the same line again and again) is summarized instead of written. Without an
// open log, Log() costs a relaxed load.

namespace forevertas::diagnostics {

bool OpenLog(const std::string &path);
bool LogOpen() noexcept;
const std::string &LogPath() noexcept;
void Log(const char *format, ...) noexcept
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 1, 2)))
#endif
        ;
// The same with no timestamp, prefix or flood control, for the crash handler
// (no allocation).
void LogRaw(const char *text) noexcept;
// The log's file descriptor (POSIX), for writing from a signal handler; -1
// without a log.
int LogDescriptor() noexcept;
// The last line of a clean exit; a log without it ended in a crash or a kill.
void LogCleanExit() noexcept;
inline constexpr char kCleanExitLine[] = "=== ForeverTAS closed normally ===";
// Every line that ends a session without a fault starts with this.
inline constexpr char kClosedPrefix[] = "=== ForeverTAS closed";

}  // namespace forevertas::diagnostics

#endif
