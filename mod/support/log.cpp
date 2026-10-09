#include "log.h"

#include <windows.h>

#include <share.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace ft::mod {

namespace {

constexpr std::size_t kMaxQueued = 1024;

struct Entry {
  std::chrono::system_clock::time_point time;
  DWORD threadId;
  std::string text;
};

struct LogState {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Entry> queue;
  std::size_t dropped = 0;
  bool open = false;
  bool stopping = false;
  std::FILE* file = nullptr;
  std::thread writer;
};

LogState& state() {
  // Deliberately never destroyed: at process exit the CRT would run its destructor with the
  // writer thread still joinable (std::terminate), after the OS has already killed the thread.
  static LogState* s = new LogState;
  return *s;
}

void writeEntry(std::FILE* f, const Entry& e) {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(e.time.time_since_epoch()).count();
  const std::time_t secs = static_cast<std::time_t>(ms / 1000);
  std::tm local{};
  localtime_s(&local, &secs);
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%H:%M:%S", &local);
  std::fprintf(f, "%s.%03lld [%5lu] %s\n", stamp, static_cast<long long>(ms % 1000),
               static_cast<unsigned long>(e.threadId), e.text.c_str());
}

void writerLoop() {
  LogState& s = state();
  std::unique_lock lock(s.mutex);
  for (;;) {
    s.wake.wait(lock, [&] { return s.stopping || !s.queue.empty(); });
    std::deque<Entry> batch;
    batch.swap(s.queue);
    const std::size_t dropped = std::exchange(s.dropped, 0);
    const bool stop = s.stopping;
    lock.unlock();
    for (const Entry& e : batch) writeEntry(s.file, e);
    if (dropped) std::fprintf(s.file, "(%zu log lines dropped)\n", dropped);
    std::fflush(s.file);
    lock.lock();
    if (stop && s.queue.empty()) return;
  }
}

}  // namespace

void openLog(const std::filesystem::path& file) {
  LogState& s = state();
  std::lock_guard lock(s.mutex);
  if (s.open) return;
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  s.file = _wfsopen(file.c_str(), L"w", _SH_DENYWR);  // readable while the game runs
  if (!s.file) return;
  s.open = true;
  s.writer = std::thread(writerLoop);
}

void logLine(std::string line) {
  LogState& s = state();
  {
    std::lock_guard lock(s.mutex);  // the writer holds it only to swap queues, never during I/O
    if (!s.open || s.stopping) return;
    if (s.queue.size() >= kMaxQueued) {
      ++s.dropped;
      return;
    }
    s.queue.push_back({std::chrono::system_clock::now(), GetCurrentThreadId(), std::move(line)});
  }
  state().wake.notify_one();
}

void closeLog() {
  LogState& s = state();
  {
    std::lock_guard lock(s.mutex);
    if (!s.open) return;
    s.stopping = true;
  }
  s.wake.notify_one();
  if (s.writer.joinable()) s.writer.join();
  std::fclose(s.file);
  s.file = nullptr;
  s.open = false;
  s.stopping = false;
}

}  // namespace ft::mod
