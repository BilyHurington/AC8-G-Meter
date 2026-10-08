#include "sources/csv_recorder.h"

namespace ft {

bool CsvRecorder::open(const std::string& path, std::string* error) {
  close();
  // Binary mode: '\n' line endings on every platform, so logs diff cleanly.
  file_.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
  if (!file_.is_open()) {
    if (error) *error = "cannot open '" + path + "' for writing";
    return false;
  }
  path_ = path;
  rows_ = 0;
  file_ << csvHeader() << '\n';
  return true;
}

void CsvRecorder::write(const CsvRow& row) {
  if (!file_.is_open()) return;
  file_ << formatCsvRow(row) << '\n';
  ++rows_;
}

void CsvRecorder::writeComment(std::string_view text) {
  if (!file_.is_open()) return;
  file_ << "# " << text << '\n';
}

void CsvRecorder::close() {
  if (!file_.is_open()) return;
  file_.flush();
  file_.close();
}

}  // namespace ft
