#pragma once
// "Mission history" window: the missions saved by a MissionArchive, newest first. Clicking a
// row loads it (asynchronously) for the summary window; rows can be deleted after a
// confirmation. Never blocks on file I/O: listing, loading and deleting run on the archive's
// worker thread.
#include <future>
#include <memory>
#include <optional>
#include <vector>

#include "storage/mission_archive.h"

namespace ft {

class MissionHistoryWindow {
 public:
  struct Result {
    std::shared_ptr<const MissionReport> report;  // a mission finished loading: show it
    MissionIndexEntry entry;                      // ... and where it came from
  };

  // Draws the window while `open` is true. Re-lists the archive whenever the window opens.
  Result draw(MissionArchive& archive, bool& open);
  bool loading() const { return pending_.valid(); }

 private:
  bool wasOpen_ = false;
  std::uint64_t seenVersion_ = 0;
  MissionIndex index_;
  std::future<LoadedMission> pending_;
  std::filesystem::path pendingPath_;  // row being loaded
  std::string loadError_;
  std::optional<MissionIndexEntry> confirmDelete_;
};

}  // namespace ft
