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

  // Draws the window while `open` is true. Re-lists the archive whenever the window opens
  // ("Reading folder..." until that listing is done). Closing the window abandons a load in
  // progress: its result is dropped, never shown later.
  Result draw(MissionArchive& archive, bool& open);
  // Starts loading `entry` (what clicking its row does). A load already in progress is
  // abandoned: the most recent click wins.
  void requestLoad(MissionArchive& archive, const MissionIndexEntry& entry);
  bool loading() const { return pending_.valid(); }
  // The listing shown is being re-read (as of the last draw).
  bool readingFolder() const { return index_.refreshing; }

 private:
  void cancelLoad();

  bool wasOpen_ = false;
  std::uint64_t seenVersion_ = 0;
  MissionIndex index_;
  std::future<LoadedMission> pending_;
  std::filesystem::path pendingPath_;  // row being loaded
  std::string loadError_;
  std::optional<MissionIndexEntry> confirmDelete_;
};

}  // namespace ft
