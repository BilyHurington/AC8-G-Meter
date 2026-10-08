#pragma once
// CSV flight-log format shared by CsvRecorder and ReplaySource.
//
// Header (exact column names, order fixed, '#' lines are comments):
//   t,px,py,pz,vx,vy,vz,fx,fy,fz,rx,ry,rz,ux,uy,uz,phase,true_nz,true_aoa
// - velocity columns may be empty (source without velocity)
// - phase is one of: none, active, paused, ended
// - true_nz / true_aoa are optional ground truth (empty when unknown)
#include <optional>
#include <string>
#include <string_view>

#include "core/sample.h"

namespace ft {

struct CsvRow {
  RawFlightSample sample;
  std::optional<double> trueNz;
  std::optional<double> trueAoaDeg;
};

std::string_view csvHeader();
std::string formatCsvRow(const CsvRow& row);
// Returns nullopt (and sets *error if given) for malformed lines.
std::optional<CsvRow> parseCsvRow(std::string_view line, std::string* error = nullptr);

}  // namespace ft
