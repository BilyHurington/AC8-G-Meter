#include "sources/csv_recorder.h"

#include "core/path_utf8.h"

namespace ft {

bool CsvRecorder::open(const std::filesystem::path& path, std::string* error) {
  close();
  // A larger stream buffer (about 6 s of 60 Hz rows) so writes reach the OS in big blocks.
  // libstdc++ takes it only before open(), MSVC only after it (before any I/O).
  buffer_.resize(64 * 1024);
  const auto setBuffer = [this] { file_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size())); };
#if !defined(_MSC_VER)
  setBuffer();
#endif
  // Binary mode: '\n' line endings on every platform, so logs diff cleanly.
  file_.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
  if (!file_.is_open()) {
    if (error) *error = "cannot open '" + pathToUtf8(path) + "' for writing";
    return false;
  }
#if defined(_MSC_VER)
  setBuffer();
#endif
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

void CsvRecorder::flush() {
  if (file_.is_open()) file_.flush();
}

void CsvRecorder::close() {
  if (!file_.is_open()) return;
  file_.flush();
  file_.close();
}

}  // namespace ft
