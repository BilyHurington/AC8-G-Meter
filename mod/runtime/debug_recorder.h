#pragma once
// Debug recording (Settings::diagnostics.debugRecording, game thread): one CSV row per Lua
// frame with everything the game reported (the base frame and the debug block), what the
// sampler pushed into the pipeline, what the pipeline computed, and a G from a plain finite
// difference of the raw velocity (no gate, no filter) to check the pipeline against.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "core/metrics.h"
#include "core/sample.h"
#include "flight/lua_frame.h"

namespace ft::mod {

class DebugRecorder {
 public:
  // A new file <dir>\ac8_debug_<local time>.csv. False with *error on failure.
  bool open(const std::filesystem::path& dir, std::string* error);
  void close();
  bool isOpen() const { return file_.is_open(); }
  const std::filesystem::path& path() const { return path_; }
  std::uint64_t rows() const { return rows_; }

  // `pushed`: the samples the sampler produced for this frame; `snapshot`: the pipeline after
  // pushing them.
  void write(const LuaFrame& frame, std::span<const RawFlightSample> pushed, const MetricsSnapshot& snapshot);

 private:
  std::vector<char> buffer_;  // outlives file_
  std::ofstream file_;
  std::filesystem::path path_;
  std::uint64_t rows_ = 0;
  // The previous frame with a plane, for the finite difference.
  bool hasPrevious_ = false;
  double previousT_ = 0.0;
  double previousPawn_ = 0.0;
  Vec3 previousVelocity_;  // m/s
};

}  // namespace ft::mod
