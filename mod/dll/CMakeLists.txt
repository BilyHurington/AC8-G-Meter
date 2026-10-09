# Import library for the UE4SS.dll exports the Lua bridge uses, generated from the .def file
# (no UE4SS build or headers needed).
set(_ue4ss_def ${CMAKE_CURRENT_SOURCE_DIR}/ue4ss_lua.def)
set(_ue4ss_lib ${CMAKE_CURRENT_BINARY_DIR}/ue4ss_lua.lib)
add_custom_command(OUTPUT ${_ue4ss_lib}
  COMMAND ${CMAKE_AR} /nologo /def:${_ue4ss_def} /machine:x64 /out:${_ue4ss_lib}
  DEPENDS ${_ue4ss_def}
  COMMENT "Generating the UE4SS Lua wrapper import library")
add_custom_target(ue4ss_lua_implib DEPENDS ${_ue4ss_lib})

# (ue4ss_lua.def is not a source: CMake would use it as this DLL's own export list.)
add_library(ac8gmeter SHARED mod_main.cpp lua_view.h)
set_target_properties(ac8gmeter PROPERTIES PREFIX "" OUTPUT_NAME ac8gmeter)
add_dependencies(ac8gmeter ue4ss_lua_implib)
target_link_libraries(ac8gmeter PRIVATE ft_mod_runtime ${_ue4ss_lib} delayimp bcrypt)
# UE4SS.dll is loaded only on first use, so the DLL also loads outside the game (tests).
target_link_options(ac8gmeter PRIVATE /DELAYLOAD:UE4SS.dll)
ft_warnings(ac8gmeter)
