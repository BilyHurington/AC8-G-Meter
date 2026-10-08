#pragma once
// Plays a CSV flight log back in (scaled) real time.
#include <filesystem>
#include <string>
#include <vector>

#include "core/source.h"
#include "sources/csv_format.h"

namespace ft {

// Reads a log: '#' comment lines and blank lines are skipped, the first remaining line must
// be the exact header. Errors name the offending line number.
bool loadCsvLog(const std::filesystem::path& path, std::vector<CsvRow>& rows, std::string* error = nullptr);

class ReplaySource final : public ITelemetrySource {
 public:
  bool load(const std::filesystem::path& path, std::string* error = nullptr);
  void setRows(std::vector<CsvRow> rows);  // e.g. generated in memory

  void poll(double realDtS, std::vector<RawFlightSample>& out) override;
  std::string_view name() const override { return "Replay"; }

  void restart();
  void setTimeScale(double scale) { timeScale_ = scale; }
  double timeScale() const { return timeScale_; }
  void setPaused(bool paused) { paused_ = paused; }
  bool paused() const { return paused_; }
  void setLoop(bool loop) { loop_ = loop; }
  bool loop() const { return loop_; }

  bool finished() const;
  double progress() const;      // 0..1
  double durationS() const;
  const std::vector<CsvRow>& rows() const { return rows_; }
  // Most recently emitted row (null before the first poll) for its ground truth. Its
  // sample.t is the original log time, without the loop offset.
  const CsvRow* lastEmitted() const;
  const std::filesystem::path& path() const { return path_; }  // empty for setRows()

 private:
  std::vector<CsvRow> rows_;
  std::filesystem::path path_;
  std::size_t next_ = 0;
  double playhead_ = 0.0;       // seconds since first row
  double timeScale_ = 1.0;
  bool paused_ = false;
  bool loop_ = false;
  // Looping restarts game time from 0; offset keeps emitted timestamps monotonic.
  double loopTimeOffset_ = 0.0;
  static constexpr std::size_t kNone = static_cast<std::size_t>(-1);
  std::size_t lastEmitted_ = kNone;
};

}  // namespace ft
