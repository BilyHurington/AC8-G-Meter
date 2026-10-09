// A fake UE4SS.dll for the mod's end-to-end test: exports the RC::LuaMadeSimple::Lua members
// the mod imports (same signatures, so the same decorated names as mod/dll/ue4ss_lua.def),
// implemented on fake_lua.h's number stack with UE4SS's semantics (get_number removes the
// value it reads).
#include <cstddef>
#include <cstdint>

#include "fake_lua.h"

namespace RC::LuaMadeSimple {

class __declspec(dllexport) Lua {
 public:
  explicit Lua(lua_State* state);
  ~Lua();
  Lua(const Lua&) = delete;
  Lua& operator=(const Lua&) = delete;

  std::int32_t get_stack_size() const;
  bool is_number(std::int32_t index) const;
  double get_number(std::int32_t index) const;
  void set_number(double value) const;

 private:
  std::size_t slot(std::int32_t index) const;  // Lua stack index -> vector index

  lua_State* state_;
  unsigned char layout_[0xA0];  // the real class is 0xA8 bytes; the mod must leave room
};

Lua::Lua(lua_State* state) : state_(state), layout_{} {}
Lua::~Lua() = default;

std::size_t Lua::slot(std::int32_t index) const {
  const auto n = static_cast<std::int32_t>(state_->stack.size());
  return static_cast<std::size_t>(index < 0 ? n + index : index - 1);
}

std::int32_t Lua::get_stack_size() const { return static_cast<std::int32_t>(state_->stack.size()); }

bool Lua::is_number(std::int32_t index) const { return slot(index) < state_->stack.size(); }

double Lua::get_number(std::int32_t index) const {
  const std::size_t i = slot(index);
  const double v = state_->stack[i];
  state_->stack.erase(state_->stack.begin() + static_cast<std::ptrdiff_t>(i));
  return v;
}

void Lua::set_number(double value) const { state_->stack.push_back(value); }

}  // namespace RC::LuaMadeSimple
