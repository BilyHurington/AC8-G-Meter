# In-game mod (Windows only): a UE4SS Lua mod (lua/) that samples the player's aircraft on
# the game thread and passes the numbers to the native DLL ac8gmeter.dll, which runs the
# telemetry pipeline and draws the overlay into AC8's DX12 swap chain.
#
#   ft_mod_support : log file
#   ft_mod_overlay : DXGI Present hook, ImGui context + Win32/DX12 backends, input forwarding
#   ft_mod_flight  : Lua frame protocol -> RawFlightSample + mission phase (pure, unit-tested)
#   ft_mod_runtime : pipeline, settings, archive, CSV recording and UI wiring
#   ac8gmeter      : the DLL: exports called from Lua, UE4SS Lua bridge, DllMain

add_library(ft_mod_support STATIC support/log.cpp support/log.h)
target_include_directories(ft_mod_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ft_mod_support PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
ft_warnings(ft_mod_support)

file(GLOB _overlay_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/overlay/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/overlay/*.h)
add_library(ft_mod_overlay STATIC ${_overlay_srcs})
set_target_properties(ft_mod_overlay PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(ft_mod_overlay PUBLIC ft_mod_support imgui_win32_dx12)
ft_warnings(ft_mod_overlay)

file(GLOB _flight_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/flight/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/flight/*.h)
add_library(ft_mod_flight STATIC ${_flight_srcs})
target_include_directories(ft_mod_flight PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ft_mod_flight PUBLIC ft_core)
ft_warnings(ft_mod_flight)

file(GLOB _runtime_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.h)
add_library(ft_mod_runtime STATIC ${_runtime_srcs})
target_link_libraries(ft_mod_runtime PUBLIC ft_mod_flight ft_mod_overlay ft_ui ft_sources ft_config ft_storage ft_core)
ft_warnings(ft_mod_runtime)

add_subdirectory(dll)

# Mod folder ready to copy into Game\Binaries\Win64\ue4ss\Mods\: build/mod_package/AC8GMeter
set(FT_MOD_PACKAGE_DIR ${CMAKE_BINARY_DIR}/mod_package/AC8GMeter)
file(GLOB _mod_lua CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lua/Scripts/*.lua)
add_custom_target(ac8gmeter_package ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${FT_MOD_PACKAGE_DIR}/Scripts ${FT_MOD_PACKAGE_DIR}/assets/fonts
  COMMAND ${CMAKE_COMMAND} -E touch ${FT_MOD_PACKAGE_DIR}/enabled.txt
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_mod_lua} $<TARGET_FILE:ac8gmeter> ${FT_MOD_PACKAGE_DIR}/Scripts/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/package/README.txt
          ${CMAKE_CURRENT_SOURCE_DIR}/package/THIRD_PARTY_NOTICES.txt ${FT_MOD_PACKAGE_DIR}/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/assets/fonts/B612Mono-Regular.ttf
          ${CMAKE_SOURCE_DIR}/assets/fonts/OFL.txt ${FT_MOD_PACKAGE_DIR}/assets/fonts/
  DEPENDS ac8gmeter ${_mod_lua}
  COMMENT "Assembling the mod folder in ${FT_MOD_PACKAGE_DIR}")
