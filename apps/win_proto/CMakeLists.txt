add_executable(ac8_gmeter_proto main.cpp d3d12_host.cpp d3d12_host.h wic_png.cpp wic_png.h)
target_link_libraries(ac8_gmeter_proto PRIVATE ft_proto_common imgui_win32_dx12 windowscodecs ole32 shell32)
target_compile_definitions(ac8_gmeter_proto PRIVATE FT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
ft_warnings(ac8_gmeter_proto)
ft_copy_proto_assets(ac8_gmeter_proto)
