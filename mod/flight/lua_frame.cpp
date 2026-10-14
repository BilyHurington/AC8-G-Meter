#include "flight/lua_frame.h"

#include <algorithm>
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

const char* luaDebugFieldName(LuaDebugField field) {
  switch (field) {
    case LuaDebugField::RealTimeS: return "real_time_s";
    case LuaDebugField::GlobalTimeDilation: return "global_time_dilation";
    case LuaDebugField::CustomTimeDilation: return "custom_time_dilation";
    case LuaDebugField::Pitch: return "pitch_deg";
    case LuaDebugField::Yaw: return "yaw_deg";
    case LuaDebugField::Roll: return "roll_deg";
    case LuaDebugField::MoveVelX: return "move_vel_x_cms";
    case LuaDebugField::MoveVelY: return "move_vel_y_cms";
    case LuaDebugField::MoveVelZ: return "move_vel_z_cms";
    case LuaDebugField::GameSpeedKmh: return "game_speed_kmh";
    case LuaDebugField::GameSpeedMps: return "game_speed_mps";
    case LuaDebugField::GameAltitudeM: return "game_altitude_m";
    case LuaDebugField::ImpactCamera: return "impact_camera";
    case LuaDebugField::InputIgnored: return "input_ignored";
    case LuaDebugField::ViewTargetIsPlane: return "view_target_is_plane";
    case LuaDebugField::WorldToMeters: return "world_to_meters";
    case LuaDebugField::Count: break;
  }
  return "?";
}

std::optional<LuaFrame> parseLuaFrame(std::span<const double> all, std::string* error) {
  const std::size_t base = static_cast<std::size_t>(kLuaFieldCount);
  const std::size_t withDebug = base + static_cast<std::size_t>(kLuaDebugFieldCount);
  if (all.size() != base && all.size() != withDebug) {
    fail(error, "expected " + std::to_string(base) + " or " + std::to_string(withDebug) + " numbers, got " +
                    std::to_string(all.size()));
    return std::nullopt;
  }
  const std::span<const double> values = all.first(base);
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
  if (all.size() == withDebug) {
    f.debug.emplace();
    std::copy(all.begin() + static_cast<std::ptrdiff_t>(base), all.end(), f.debug->begin());
  }
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
