#pragma once
// The numbers the Lua sampler (mod/lua/Scripts/main.lua) passes to ac8gmeter_frame once per
// game tick, in this order. Keep both sides in step; bump kLuaFrameVersion on any change.
#include <optional>
#include <span>
#include <string>

#include "core/math.h"

namespace ft::mod {

inline constexpr int kLuaFrameVersion = 1;

enum class LuaField : int {
  Version,      // kLuaFrameVersion
  WorldTimeS,   // UGameplayStatics::GetTimeSeconds: game time, stops while paused
  DeltaS,       // UGameplayStatics::GetWorldDeltaSeconds (0 when unknown)
  Paused,       // 0 / 1: UGameplayStatics::IsGamePaused
  HasPlane,     // 0 / 1: the player controller possesses an ALivePlayerPlane
  PawnId,       // the plane's address (identity only; changes on respawn / new mission)
  LocX, LocY, LocZ,           // K2_GetActorLocation, cm (UE world: X forward, Y right, Z up)
  VelX, VelY, VelZ,           // GetVelocity, cm/s
  FwdX, FwdY, FwdZ,           // GetActorForwardVector
  RightX, RightY, RightZ,     // GetActorRightVector
  UpX, UpY, UpZ,              // GetActorUpVector
  Count
};
inline constexpr int kLuaFieldCount = static_cast<int>(LuaField::Count);

struct LuaFrame {
  double worldTimeS = 0.0;
  double deltaS = 0.0;
  bool paused = false;
  bool hasPlane = false;
  double pawnId = 0.0;
  Vec3 locationCm;
  Vec3 velocityCmS;
  Basis attitude;  // body axes in world space
};

// Validates the version, the count and every value (finite, flags 0/1, unit axes when a
// plane is present). nullopt with *error describing the first problem.
std::optional<LuaFrame> parseLuaFrame(std::span<const double> values, std::string* error = nullptr);

}  // namespace ft::mod
