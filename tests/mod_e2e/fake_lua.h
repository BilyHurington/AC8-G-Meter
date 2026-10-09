#pragma once
// Stand-in for the Lua state the mod's exports receive, shared by the fake UE4SS.dll and the
// end-to-end test: a plain stack of numbers (index -1 = top, 1 = bottom, as in Lua).
#include <vector>

struct lua_State {
  std::vector<double> stack;
};
