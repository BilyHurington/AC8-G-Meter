#pragma once
// MissionArchive: saves finished missions to a directory (format in mission_files.h), lists
// and loads them back, all on one background worker thread, so the callers (the game thread
// through MissionArchiveRecorder, the render thread through the history window) never block
// on file I/O.
//
// Thread safety: every public member may be called from any thread. Jobs run one at a time
// in submission order on the worker; results come back as copies (lastSave(), index()) or
// through a std::future (loadAsync()). The destructor runs every queued job (a save is never
// dropped) and joins the worker.
//
// Errors never throw: they end up in SaveStatus::error, MissionIndex::error or
// LoadedMission::error.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/metrics.h"
#include "storage/mission_files.h"

namespace ft {

struct SaveStatus {
  enum class State : std::uint8_t { None, Pending, Saved, Failed };
  State state = State::None;
  std::shared_ptr<const MissionReport> report;  // the report this status is about
  std::filesystem::path path;                   // the .json file (Saved)
  std::string error;                            // Failed
};

struct MissionIndex {
  std::vector<MissionIndexEntry> entries;  // newest first
  std::uint64_t version = 0;               // bumped by every completed listing
  bool refreshing = false;                 // a listing is queued or running
  std::string error;                       // last listing / delete problem, "" if none
};

struct LoadedMission {
  MissionIndexEntry entry;
  std::shared_ptr<const MissionReport> report;  // null on failure
  std::string error;
};

class MissionArchive {
 public:
  // maxSaved: missions kept after a save, oldest deleted first (0 = unlimited).
  explicit MissionArchive(std::filesystem::path directory, int maxSaved = 50);
  ~MissionArchive();
  MissionArchive(const MissionArchive&) = delete;
  MissionArchive& operator=(const MissionArchive&) = delete;

  const std::filesystem::path& directory() const { return dir_; }
  void setMaxSaved(int maxSaved) { maxSaved_.store(maxSaved < 0 ? 0 : maxSaved); }

  // Writes "<stamp>_m<id>.trace.csv" then "<stamp>_m<id>.json" (each via temp file + rename),
  // applies the retention limit and refreshes the index.
  void saveAsync(std::shared_ptr<const MissionReport> report);
  SaveStatus lastSave() const;

  // Re-reads the directory (JSON files only, corrupt ones skipped) into index().
  void refreshAsync();
  MissionIndex index() const;
  // index().entries.size() without the copy.
  std::size_t indexSize() const;
  std::uint64_t indexVersion() const { return indexVersion_.load(); }

  // Stats + trace of one archived mission. Poll the future with wait_for(0).
  std::future<LoadedMission> loadAsync(const MissionIndexEntry& entry);
  // Deletes both files of an entry, then refreshes the index.
  void removeAsync(const MissionIndexEntry& entry);

  // Blocks until every job queued so far has finished (tests, shutdown).
  void flush();

 private:
  void post(std::function<void()> job);
  void workerLoop();
  void doSave(const std::shared_ptr<const MissionReport>& report);
  void doList();
  void applyRetention();

  const std::filesystem::path dir_;
  std::atomic<int> maxSaved_;
  std::atomic<std::uint64_t> indexVersion_{0};

  mutable std::mutex mutex_;  // guards everything below
  std::condition_variable wake_, idle_;
  std::deque<std::function<void()>> jobs_;
  bool busy_ = false;
  bool stop_ = false;
  SaveStatus lastSave_;
  MissionIndex index_;
  int pendingLists_ = 0;

  std::thread worker_;  // last: started after everything above is initialised
};

}  // namespace ft
