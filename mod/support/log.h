#pragma once
// Mod log file. Writing happens on a background thread: logLine() never blocks the game or
// render thread on file I/O (lines are dropped while the queue is full or the log closed).
#include <filesystem>
#include <format>
#include <string>
#include <utility>

namespace ft::mod {

void openLog(const std::filesystem::path& file);  // truncates; later calls are ignored
void logLine(std::string line);                   // timestamped and thread-tagged by the writer
void closeLog();                                  // flushes and joins the writer thread

template <class... Args>
void logf(std::format_string<Args...> fmt, Args&&... args) {
  logLine(std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace ft::mod
