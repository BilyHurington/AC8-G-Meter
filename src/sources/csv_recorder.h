#pragma once
#include <fstream>
#include <optional>
#include <string>

#include "sources/csv_format.h"

namespace ft {

class CsvRecorder {
 public:
  ~CsvRecorder() { close(); }
  bool open(const std::string& path, std::string* error = nullptr);  // truncates
  void write(const CsvRow& row);
  void close();
  bool isOpen() const { return file_.is_open(); }
  const std::string& path() const { return path_; }
  std::size_t rowsWritten() const { return rows_; }

 private:
  std::ofstream file_;
  std::string path_;
  std::size_t rows_ = 0;
};

}  // namespace ft
