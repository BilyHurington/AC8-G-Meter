#pragma once
// The part of UE4SS's exported Lua wrapper (RC::LuaMadeSimple::Lua, MIT licensed, from
// RE-UE4SS deps/first/LuaMadeSimple) the mod uses to read and return numbers on the Lua stack
// of a function called from Lua. Only these members are declared, with the exact signatures
// (and so the MSVC-mangled names, see ue4ss_lua.def) of the exports:
//   Lua(lua_State*)       stores the state (and a registry back-reference)
//   get_stack_size()      lua_gettop
//   is_number(i)          lua_isnumber
//   get_number(i)         lua_tonumber, then lua_remove(i)
//   set_number(v)         lua_pushnumber
// The object is constructed by UE4SS's own constructor into `storage_`. The real class is
// 0xA8 bytes in the AC8 builds (Release STL); the buffer is larger so that a future build
// with more members cannot overrun it.
#include <cstdint>

struct lua_State;

namespace RC::LuaMadeSimple {

class Lua {
 public:
  __declspec(dllimport) explicit Lua(lua_State* state);
  __declspec(dllimport) ~Lua();
  Lua(const Lua&) = delete;
  Lua& operator=(const Lua&) = delete;

  __declspec(dllimport) std::int32_t get_stack_size() const;
  __declspec(dllimport) bool is_number(std::int32_t index) const;
  __declspec(dllimport) double get_number(std::int32_t index) const;
  __declspec(dllimport) void set_number(double value) const;

 private:
  alignas(16) unsigned char storage_[0x400];
};

}  // namespace RC::LuaMadeSimple
