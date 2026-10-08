#include "sources/csv_format.h"

#include <array>
#include <charconv>
#include <string>
#include <vector>

namespace ft {

namespace {

constexpr int kColumnCount = 19;
constexpr int kPrecision = 6;  // decimals: micro-seconds / micro-metres, plenty for 1e-3 G

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// std::to_chars / from_chars are locale independent (a German locale would write "1,5").
void appendNumber(std::string& out, double v) {
  std::array<char, 64> buf{};
  const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed,
                                 kPrecision);
  if (res.ec == std::errc()) {
    out.append(buf.data(), res.ptr);
  } else {  // only absurdly large values do not fit; fall back to scientific
    const auto sci = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    out.append(buf.data(), sci.ptr);
  }
}

bool parseNumber(std::string_view field, double& v) {
  if (!field.empty() && field.front() == '+') field.remove_prefix(1);  // from_chars rejects '+'
  if (field.empty()) return false;
  const auto res = std::from_chars(field.data(), field.data() + field.size(), v);
  return res.ec == std::errc() && res.ptr == field.data() + field.size();
}

bool parsePhase(std::string_view s, MissionPhase& phase) {
  for (MissionPhase p : {MissionPhase::None, MissionPhase::Active, MissionPhase::Paused,
                         MissionPhase::Ended}) {
    if (s == toString(p)) {
      phase = p;
      return true;
    }
  }
  return false;
}

std::optional<CsvRow> fail(std::string* error, std::string msg) {
  if (error) *error = std::move(msg);
  return std::nullopt;
}

}  // namespace

std::string_view csvHeader() {
  return "t,px,py,pz,vx,vy,vz,fx,fy,fz,rx,ry,rz,ux,uy,uz,phase,true_nz,true_aoa";
}

std::string formatCsvRow(const CsvRow& row) {
  const RawFlightSample& s = row.sample;
  std::string out;
  out.reserve(256);
  auto num = [&out](double v) {
    appendNumber(out, v);
    out += ',';
  };
  auto vec = [&num](const Vec3& v) {
    num(v.x);
    num(v.y);
    num(v.z);
  };
  num(s.t);
  vec(s.position);
  if (s.velocity) vec(*s.velocity);
  else out += ",,,";
  vec(s.attitude.forward);
  vec(s.attitude.right);
  vec(s.attitude.up);
  out += toString(s.phase);
  out += ',';
  if (row.trueNz) appendNumber(out, *row.trueNz);
  out += ',';
  if (row.trueAoaDeg) appendNumber(out, *row.trueAoaDeg);
  return out;
}

std::optional<CsvRow> parseCsvRow(std::string_view line, std::string* error) {
  std::vector<std::string_view> fields;
  fields.reserve(kColumnCount);
  for (;;) {
    const std::size_t comma = line.find(',');
    fields.push_back(trim(line.substr(0, comma)));
    if (comma == std::string_view::npos) break;
    line.remove_prefix(comma + 1);
  }
  if (fields.size() != kColumnCount)
    return fail(error, "expected " + std::to_string(kColumnCount) + " columns, got " +
                           std::to_string(fields.size()));

  static constexpr std::array<std::string_view, kColumnCount> kNames{
      "t", "px", "py", "pz", "vx", "vy", "vz", "fx", "fy", "fz",
      "rx", "ry", "rz", "ux", "uy", "uz", "phase", "true_nz", "true_aoa"};
  std::array<double, kColumnCount> v{};
  std::array<bool, kColumnCount> present{};
  for (int i = 0; i < kColumnCount; ++i) {
    if (i == 16) continue;  // phase
    if (fields[i].empty()) continue;
    if (!parseNumber(fields[i], v[i]))
      return fail(error, "bad number in column '" + std::string(kNames[i]) + "': '" +
                             std::string(fields[i]) + "'");
    present[i] = true;
  }
  // Required: t, position, attitude. Velocity is all-or-nothing.
  for (int i : {0, 1, 2, 3, 7, 8, 9, 10, 11, 12, 13, 14, 15})
    if (!present[i]) return fail(error, "missing value in column '" + std::string(kNames[i]) + "'");
  if (present[4] != present[5] || present[4] != present[6])
    return fail(error, "velocity columns must be all present or all empty");

  CsvRow row;
  RawFlightSample& s = row.sample;
  if (!parsePhase(fields[16], s.phase))
    return fail(error, "bad phase '" + std::string(fields[16]) +
                           "' (expected none, active, paused or ended)");
  s.t = v[0];
  s.position = {v[1], v[2], v[3]};
  if (present[4]) s.velocity = Vec3{v[4], v[5], v[6]};
  s.attitude.forward = {v[7], v[8], v[9]};
  s.attitude.right = {v[10], v[11], v[12]};
  s.attitude.up = {v[13], v[14], v[15]};
  if (present[17]) row.trueNz = v[17];
  if (present[18]) row.trueAoaDeg = v[18];
  return row;
}

}  // namespace ft
