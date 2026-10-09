#include "flight/lua_frame.h"

#include <cmath>

namespace ft::mod {

namespace {

double at(std::span<const double> v, LuaField f) { return v[static_cast<std::size_t>(f)]; }

Vec3 vec(std::span<const double> v, LuaField x) {
  const auto i = static_cast<std::size_t>(x);
  return {v[i], v[i + 1], v[i + 2]};
}

bool isFlag(double v) { return v == 0.0 || v == 1.0; }

bool isUnit(const Vec3& v) { return std::abs(length(v) - 1.0) < 0.01; }  // UE axes are float

bool fail(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}

}  // namespace

std::optional<LuaFrame> parseLuaFrame(std::span<const double> values, std::string* error) {
  if (values.size() != static_cast<std::size_t>(kLuaFieldCount)) {
    fail(error, "expected " + std::to_string(kLuaFieldCount) + " numbers, got " + std::to_string(values.size()));
    return std::nullopt;
  }
  if (at(values, LuaField::Version) != kLuaFrameVersion) {
    fail(error, "protocol version " + std::to_string(at(values, LuaField::Version)) + ", expected " +
                    std::to_string(kLuaFrameVersion));
    return std::nullopt;
  }
  for (std::size_t i = 0; i < values.size(); ++i)
    if (!std::isfinite(values[i])) {
      fail(error, "value " + std::to_string(i) + " is not finite");
      return std::nullopt;
    }
  if (!isFlag(at(values, LuaField::Paused)) || !isFlag(at(values, LuaField::HasPlane))) {
    fail(error, "flags must be 0 or 1");
    return std::nullopt;
  }

  LuaFrame f;
  f.worldTimeS = at(values, LuaField::WorldTimeS);
  f.deltaS = at(values, LuaField::DeltaS);
  f.paused = at(values, LuaField::Paused) != 0.0;
  f.hasPlane = at(values, LuaField::HasPlane) != 0.0;
  if (f.deltaS < 0.0) {
    fail(error, "negative delta time");
    return std::nullopt;
  }
  if (!f.hasPlane) return f;  // the remaining fields are placeholders

  f.pawnId = at(values, LuaField::PawnId);
  f.locationCm = vec(values, LuaField::LocX);
  f.velocityCmS = vec(values, LuaField::VelX);
  f.attitude.forward = vec(values, LuaField::FwdX);
  f.attitude.right = vec(values, LuaField::RightX);
  f.attitude.up = vec(values, LuaField::UpX);
  if (!isUnit(f.attitude.forward) || !isUnit(f.attitude.right) || !isUnit(f.attitude.up)) {
    fail(error, "aircraft axes are not unit vectors");
    return std::nullopt;
  }
  return f;
}

}  // namespace ft::mod
