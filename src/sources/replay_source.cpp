#include "sources/replay_source.h"

#include <algorithm>
#include <fstream>

#include "core/path_utf8.h"

namespace ft {

bool loadCsvLog(const std::filesystem::path& path, std::vector<CsvRow>& rows, std::string* error) {
  const std::string name = pathToUtf8(path);  // for error messages
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    if (error) *error = "cannot open '" + name + "'";
    return false;
  }
  rows.clear();
  bool headerSeen = false;
  std::string line;
  for (int lineNo = 1; std::getline(file, line); ++lineNo) {
    std::string_view s = line;
    if (!s.empty() && s.back() == '\r') s.remove_suffix(1);
    const std::size_t first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos || s[first] == '#') continue;
    if (!headerSeen) {
      if (s != csvHeader()) {
        if (error) *error = name + ":" + std::to_string(lineNo) + ": unexpected header (expected '" +
                            std::string(csvHeader()) + "')";
        return false;
      }
      headerSeen = true;
      continue;
    }
    std::string rowError;
    std::optional<CsvRow> row = parseCsvRow(s, &rowError);
    if (!row) {
      if (error) *error = name + ":" + std::to_string(lineNo) + ": " + rowError;
      return false;
    }
    rows.push_back(std::move(*row));
  }
  if (!headerSeen) {
    if (error) *error = name + ": missing header";
    return false;
  }
  return true;
}

bool ReplaySource::load(const std::filesystem::path& path, std::string* error) {
  std::vector<CsvRow> rows;
  if (!loadCsvLog(path, rows, error)) return false;
  setRows(std::move(rows));
  path_ = path;
  return true;
}

void ReplaySource::setRows(std::vector<CsvRow> rows) {
  rows_ = std::move(rows);
  path_.clear();
  restart();
}

void ReplaySource::restart() {
  next_ = 0;
  playhead_ = 0.0;
  loopTimeOffset_ = 0.0;
  lastEmitted_ = kNone;
}

void ReplaySource::poll(double realDtS, std::vector<RawFlightSample>& out) {
  if (rows_.empty() || paused_) return;
  if (finished()) return;
  playhead_ += std::max(realDtS, 0.0) * std::max(timeScale_, 0.0);

  const double t0 = rows_.front().sample.t;
  // One loop period = log duration + one average frame, so the first row of the next loop
  // does not share a timestamp with the last row of the previous one.
  const double frame = rows_.size() > 1 ? durationS() / static_cast<double>(rows_.size() - 1) : 1.0 / 60.0;
  const double period = durationS() + std::max(frame, 1e-3);

  for (;;) {
    while (next_ < rows_.size() && rows_[next_].sample.t - t0 <= playhead_) {
      RawFlightSample s = rows_[next_].sample;
      s.t += loopTimeOffset_;
      out.push_back(s);
      lastEmitted_ = next_++;
    }
    if (next_ < rows_.size() || !loop_ || playhead_ < period) break;
    next_ = 0;
    playhead_ -= period;
    loopTimeOffset_ += period;
  }
}

bool ReplaySource::finished() const { return !loop_ && next_ >= rows_.size(); }

double ReplaySource::durationS() const {
  return rows_.size() < 2 ? 0.0 : rows_.back().sample.t - rows_.front().sample.t;
}

double ReplaySource::progress() const {
  if (rows_.empty()) return 0.0;
  if (finished()) return 1.0;
  const double d = durationS();
  return d > 0.0 ? std::clamp(playhead_ / d, 0.0, 1.0) : 0.0;
}

const CsvRow* ReplaySource::lastEmitted() const {
  return lastEmitted_ < rows_.size() ? &rows_[lastEmitted_] : nullptr;
}

}  // namespace ft
